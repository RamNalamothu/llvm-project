//===- Driver.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "COFFLinkerContext.h"
#include "Config.h"
#include "DebugTypes.h"
#include "ICF.h"
#include "InputFiles.h"
#include "MarkLive.h"
#include "MinGW.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Filesystem.h"
#include "lld/Common/Timer.h"
#include "lld/Common/Version.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TarWriter.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/ToolDrivers/llvm-lib/LibDriver.h"
#include <algorithm>
#include <future>
#include <memory>
#include <optional>
#include <tuple>

using namespace lld;
using namespace lld::coff;
using namespace llvm;
using namespace llvm::object;
using namespace llvm::COFF;
using namespace llvm::sys;

COFFSyncStream::COFFSyncStream(COFFLinkerContext &ctx, DiagLevel level)
    : SyncStream(ctx.e, level), ctx(ctx) {}

COFFSyncStream coff::Log(COFFLinkerContext &ctx) {
  return {ctx, DiagLevel::Log};
}
COFFSyncStream coff::Msg(COFFLinkerContext &ctx) {
  return {ctx, DiagLevel::Msg};
}
COFFSyncStream coff::Warn(COFFLinkerContext &ctx) {
  return {ctx, DiagLevel::Warn};
}
COFFSyncStream coff::Err(COFFLinkerContext &ctx) {
  return {ctx, DiagLevel::Err};
}
COFFSyncStream coff::Fatal(COFFLinkerContext &ctx) {
  return {ctx, DiagLevel::Fatal};
}
uint64_t coff::errCount(COFFLinkerContext &ctx) { return ctx.e.errorCount; }

namespace lld::coff {

bool link(ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
          llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput) {
  // This driver-specific context will be freed later by unsafeLldMain().
  auto *ctx = new COFFLinkerContext;

  ctx->e.initialize(stdoutOS, stderrOS, exitEarly, disableOutput);
  ctx->e.logName = args::getFilenameWithoutExe(args[0]);
  ctx->e.errorLimitExceededMsg = "too many errors emitted, stopping now"
                                 " (use /errorlimit:0 to see all errors)";

  ctx->driver.linkerMain(args);

  return errCount(*ctx) == 0;
}

// Parse options of the form "old;new".
static std::pair<StringRef, StringRef>
getOldNewOptions(COFFLinkerContext &ctx, opt::InputArgList &args, unsigned id) {
  auto *arg = args.getLastArg(id);
  if (!arg)
    return {"", ""};

  StringRef s = arg->getValue();
  std::pair<StringRef, StringRef> ret = s.split(';');
  if (ret.second.empty())
    Err(ctx) << arg->getSpelling() << " expects 'old;new' format, but got "
             << s;
  return ret;
}

// Parse options of the form "old;new[;extra]".
static std::tuple<StringRef, StringRef, StringRef>
getOldNewOptionsExtra(COFFLinkerContext &ctx, opt::InputArgList &args,
                      unsigned id) {
  auto [oldDir, second] = getOldNewOptions(ctx, args, id);
  auto [newDir, extraDir] = second.split(';');
  return {oldDir, newDir, extraDir};
}

// Drop directory components and replace extension with
// ".exe", ".dll" or ".sys".
static std::string getOutputPath(StringRef path, bool isDll, bool isDriver) {
  StringRef ext = ".exe";
  if (isDll)
    ext = ".dll";
  else if (isDriver)
    ext = ".sys";

  return (sys::path::stem(path) + ext).str();
}

// Returns true if S matches /crtend.?\.o$/.
static bool isCrtend(StringRef s) {
  if (!s.consume_back(".o"))
    return false;
  if (s.ends_with("crtend"))
    return true;
  return !s.empty() && s.drop_back().ends_with("crtend");
}

// ErrorOr is not default constructible, so it cannot be used as the type
// parameter of a future.
// FIXME: We could open the file in createFutureForFile and avoid needing to
// return an error here, but for the moment that would cost us a file descriptor
// (a limited resource on Windows) for the duration that the future is pending.
using MBErrPair = std::pair<std::unique_ptr<MemoryBuffer>, std::error_code>;

// Create a std::future that opens and maps a file using the best strategy for
// the host platform.
static std::future<MBErrPair> createFutureForFile(std::string path) {
#if _WIN64
  // On Windows, file I/O is relatively slow so it is best to do this
  // asynchronously.  But 32-bit has issues with potentially launching tons
  // of threads
  auto strategy = std::launch::async;
#else
  auto strategy = std::launch::deferred;
#endif
  return std::async(strategy, [=]() {
    auto mbOrErr = MemoryBuffer::getFile(path, /*IsText=*/false,
                                         /*RequiresNullTerminator=*/false);
    if (!mbOrErr)
      return MBErrPair{nullptr, mbOrErr.getError()};
    return MBErrPair{std::move(*mbOrErr), std::error_code()};
  });
}

llvm::Triple::ArchType LinkerDriver::getArch() {
  return getMachineArchType(ctx.config.machine);
}

std::vector<Chunk *> LinkerDriver::getChunks() const {
  std::vector<Chunk *> res;
  for (ObjFile *file : ctx.objFileInstances) {
    ArrayRef<Chunk *> v = file->getChunks();
    res.insert(res.end(), v.begin(), v.end());
  }
  return res;
}

static bool compatibleMachineType(COFFLinkerContext &ctx, MachineTypes mt) {
  if (mt == IMAGE_FILE_MACHINE_UNKNOWN)
    return true;
  switch (ctx.config.machine) {
  case ARM64:
    return mt == ARM64 || mt == ARM64X;
  case ARM64EC:
  case ARM64X:
    return isAnyArm64(mt) || mt == AMD64;
  case IMAGE_FILE_MACHINE_UNKNOWN:
    return true;
  default:
    return ctx.config.machine == mt;
  }
}

void LinkerDriver::addFile(InputFile *file) {
  Log(ctx) << "Reading " << toString(file);
  if (file->lazy) {
    if (auto *f = dyn_cast<BitcodeFile>(file))
      f->parseLazy();
    else
      cast<ObjFile>(file)->parseLazy();
  } else {
    file->parse();
    if (auto *f = dyn_cast<ObjFile>(file)) {
      ctx.objFileInstances.push_back(f);
    } else if (auto *f = dyn_cast<BitcodeFile>(file)) {
      if (ltoCompilationDone) {
        Err(ctx) << "LTO object file " << toString(file)
                 << " linked in after "
                    "doing LTO compilation.";
      }
      f->symtab.bitcodeFileInstances.push_back(f);
    } else if (auto *f = dyn_cast<ImportFile>(file)) {
      ctx.importFileInstances.push_back(f);
    }
  }

  MachineTypes mt = file->getMachineType();
  // The ARM64EC target must be explicitly specified and cannot be inferred.
  if (mt == ARM64EC &&
      (ctx.config.machine == IMAGE_FILE_MACHINE_UNKNOWN ||
       (ctx.config.machineInferred &&
        (ctx.config.machine == ARM64 || ctx.config.machine == AMD64)))) {
    Err(ctx) << toString(file)
             << ": machine type arm64ec is ambiguous and cannot be "
                "inferred, use /machine:arm64ec or /machine:arm64x";
    return;
  }
  if (!compatibleMachineType(ctx, mt)) {
    Err(ctx) << toString(file) << ": machine type " << machineToStr(mt)
             << " conflicts with " << machineToStr(ctx.config.machine);
    return;
  }
  if (ctx.config.machine == IMAGE_FILE_MACHINE_UNKNOWN &&
      mt != IMAGE_FILE_MACHINE_UNKNOWN) {
    ctx.config.machineInferred = true;
    setMachine(mt);
  }

  parseDirectives(file);
}

MemoryBufferRef LinkerDriver::takeBuffer(std::unique_ptr<MemoryBuffer> mb) {
  MemoryBufferRef mbref = *mb;
  make<std::unique_ptr<MemoryBuffer>>(std::move(mb)); // take ownership

  if (ctx.driver.tar)
    ctx.driver.tar->append(relativeToRoot(mbref.getBufferIdentifier()),
                           mbref.getBuffer());
  return mbref;
}

void LinkerDriver::addBuffer(std::unique_ptr<MemoryBuffer> mb,
                             bool wholeArchive, bool lazy) {
  StringRef filename = mb->getBufferIdentifier();

  MemoryBufferRef mbref = takeBuffer(std::move(mb));

  // File type is detected by contents, not by file extension.
  switch (identify_magic(mbref.getBuffer())) {
  case file_magic::windows_resource:
    resources.push_back(mbref);
    break;
  case file_magic::archive:
    if (wholeArchive) {
      std::unique_ptr<Archive> file =
          CHECK(Archive::create(mbref), filename + ": failed to parse archive");
      Archive *archive = file.get();
      make<std::unique_ptr<Archive>>(std::move(file)); // take ownership

      int memberIndex = 0;
      for (MemoryBufferRef m : getArchiveMembers(ctx, archive)) {
        if (!archive->isThin())
          addArchiveBuffer(m, "<whole-archive>", filename, memberIndex++);
        else
          addThinArchiveBuffer(m, "<whole-archive>");
      }

      return;
    }
    addFile(make<ArchiveFile>(ctx, mbref));
    break;
  case file_magic::bitcode:
    addFile(BitcodeFile::create(ctx, mbref, "", 0, lazy));
    break;
  case file_magic::coff_object:
  case file_magic::coff_import_library:
    addFile(ObjFile::create(ctx, mbref, lazy));
    break;
  case file_magic::pdb:
    addFile(make<PDBInputFile>(ctx, mbref));
    break;
  case file_magic::coff_cl_gl_object:
    Err(ctx) << filename
             << ": is not a native COFF file. Recompile without /GL";
    break;
  case file_magic::pecoff_executable:
    if (ctx.config.mingw) {
      addFile(make<DLLFile>(ctx.symtab, mbref));
      break;
    }
    if (filename.ends_with_insensitive(".dll")) {
      Err(ctx) << filename
               << ": bad file type. Did you specify a DLL instead of an "
                  "import library?";
      break;
    }
    [[fallthrough]];
  default:
    Err(ctx) << mbref.getBufferIdentifier() << ": unknown file type";
    break;
  }
}

void LinkerDriver::enqueuePath(StringRef path, bool wholeArchive, bool lazy) {
  auto future = std::make_shared<std::future<MBErrPair>>(
      createFutureForFile(std::string(path)));
  std::string pathStr = std::string(path);
  enqueueTask([=]() {
    llvm::TimeTraceScope timeScope("File: ", path);
    auto [mb, ec] = future->get();
    if (ec) {
      // Retry reading the file (synchronously) now that we may have added
      // winsysroot search paths from SymbolTable::addFile().
      // Retrying synchronously is important for keeping the order of inputs
      // consistent.
      // This makes it so that if the user passes something in the winsysroot
      // before something we can find with an architecture, we won't find the
      // winsysroot file.
      if (std::optional<StringRef> retryPath = findFileIfNew(pathStr)) {
        auto retryMb = MemoryBuffer::getFile(*retryPath, /*IsText=*/false,
                                             /*RequiresNullTerminator=*/false);
        ec = retryMb.getError();
        if (!ec)
          mb = std::move(*retryMb);
      } else {
        // We've already handled this file.
        return;
      }
    }
    if (ec) {
      std::string msg = "could not open '" + pathStr + "': " + ec.message();
      // Check if the filename is a typo for an option flag. OptTable thinks
      // that all args that are not known options and that start with / are
      // filenames, but e.g. `/nodefaultlibs` is more likely a typo for
      // the option `/nodefaultlib` than a reference to a file in the root
      // directory.
      std::string nearest;
      if (ctx.optTable.findNearest(pathStr, nearest) > 1)
        Err(ctx) << msg;
      else
        Err(ctx) << msg << "; did you mean '" << nearest << "'";
    } else
      ctx.driver.addBuffer(std::move(mb), wholeArchive, lazy);
  });
}

void LinkerDriver::addArchiveBuffer(MemoryBufferRef mb, StringRef symName,
                                    StringRef parentName,
                                    uint64_t offsetInArchive) {
  file_magic magic = identify_magic(mb.getBuffer());
  if (magic == file_magic::coff_import_library) {
    InputFile *imp = make<ImportFile>(ctx, mb);
    imp->parentName = parentName;
    addFile(imp);
    return;
  }

  InputFile *obj;
  if (magic == file_magic::coff_object) {
    obj = ObjFile::create(ctx, mb);
  } else if (magic == file_magic::bitcode) {
    obj = BitcodeFile::create(ctx, mb, parentName, offsetInArchive,
                              /*lazy=*/false);
  } else if (magic == file_magic::coff_cl_gl_object) {
    Err(ctx) << mb.getBufferIdentifier()
             << ": is not a native COFF file. Recompile without /GL?";
    return;
  } else {
    Err(ctx) << "unknown file type: " << mb.getBufferIdentifier();
    return;
  }

  obj->parentName = parentName;
  addFile(obj);
  Log(ctx) << "Loaded " << obj << " for " << symName;
}

void LinkerDriver::addThinArchiveBuffer(MemoryBufferRef mb, StringRef symName) {
  // Pass an empty string as the archive name and an offset of 0 so that
  // the original filename is used as the buffer identifier. This is
  // useful for DTLTO, where having the member identifier be the actual
  // path on disk enables distribution of bitcode files during ThinLTO.
  addArchiveBuffer(mb, symName, /*parentName=*/"", /*OffsetInArchive=*/0);
}

void LinkerDriver::enqueueArchiveMember(const Archive::Child &c,
                                        const Archive::Symbol &sym,
                                        StringRef parentName) {

  auto reportBufferError = [=](Error &&e, StringRef childName) {
    Fatal(ctx) << "could not get the buffer for the member defining symbol "
               << &sym << ": " << parentName << "(" << childName
               << "): " << std::move(e);
  };

  if (!c.getParent()->isThin()) {
    uint64_t offsetInArchive = c.getChildOffset();
    Expected<MemoryBufferRef> mbOrErr = c.getMemoryBufferRef();
    if (!mbOrErr)
      reportBufferError(mbOrErr.takeError(), check(c.getFullName()));
    MemoryBufferRef mb = mbOrErr.get();
    enqueueTask([=]() {
      llvm::TimeTraceScope timeScope("Archive: ", mb.getBufferIdentifier());
      ctx.driver.addArchiveBuffer(mb, toCOFFString(ctx, sym), parentName,
                                  offsetInArchive);
    });
    return;
  }

  std::string childName =
      CHECK(c.getFullName(),
            "could not get the filename for the member defining symbol " +
                toCOFFString(ctx, sym));
  auto future =
      std::make_shared<std::future<MBErrPair>>(createFutureForFile(childName));
  enqueueTask([=]() {
    auto mbOrErr = future->get();
    if (mbOrErr.second)
      reportBufferError(errorCodeToError(mbOrErr.second), childName);
    llvm::TimeTraceScope timeScope("Archive: ",
                                   mbOrErr.first->getBufferIdentifier());
    ctx.driver.addThinArchiveBuffer(takeBuffer(std::move(mbOrErr.first)),
                                    toCOFFString(ctx, sym));
  });
}

bool LinkerDriver::isDecorated(StringRef sym) {
  return sym.starts_with("@") || sym.contains("@@") || sym.starts_with("?") ||
         (!ctx.config.mingw && sym.contains('@'));
}

// Parses .drectve section contents and returns a list of files
// specified by /defaultlib.
void LinkerDriver::parseDirectives(InputFile *file) {
  StringRef s = file->getDirectives();
  if (s.empty())
    return;

  Log(ctx) << "Directives: " << file << ": " << s;

  ArgParser parser(ctx);
  // .drectve is always tokenized using Windows shell rules.
  // /EXPORT: option can appear too many times, processing in fastpath.
  ParsedDirectives directives = parser.parseDirectives(s);

  for (StringRef e : directives.exports) {
    // If a common header file contains dllexported function
    // declarations, many object files may end up with having the
    // same /EXPORT options. In order to save cost of parsing them,
    // we dedup them first.
    if (!file->symtab.directivesExports.insert(e).second)
      continue;

    Export exp = parseExport(e);
    if (ctx.config.machine == I386 && ctx.config.mingw) {
      if (!isDecorated(exp.name))
        exp.name = saver().save("_" + exp.name);
      if (!exp.extName.empty() && !isDecorated(exp.extName))
        exp.extName = saver().save("_" + exp.extName);
    }
    exp.source = ExportSource::Directives;
    file->symtab.exports.push_back(exp);
  }

  // Handle /include: in bulk.
  for (StringRef inc : directives.includes)
    file->symtab.addGCRoot(inc);

  // Handle /exclude-symbols: in bulk.
  for (StringRef e : directives.excludes) {
    SmallVector<StringRef, 2> vec;
    e.split(vec, ',');
    for (StringRef sym : vec)
      excludedSymbols.insert(file->symtab.mangle(sym));
  }

  // https://docs.microsoft.com/en-us/cpp/preprocessor/comment-c-cpp?view=msvc-160
  for (auto *arg : directives.args) {
    switch (arg->getOption().getID()) {
    case OPT_aligncomm:
      file->symtab.parseAligncomm(arg->getValue());
      break;
    case OPT_alternatename:
      file->symtab.parseAlternateName(arg->getValue());
      break;
    case OPT_arm64xsameaddress:
      if (!file->symtab.isEC())
        Warn(ctx) << arg->getSpelling()
                  << " is not allowed in non-ARM64EC files (" << toString(file)
                  << ")";
      break;
    case OPT_defaultlib:
      if (std::optional<StringRef> path = findLibIfNew(arg->getValue()))
        enqueuePath(*path, false, false);
      break;
    case OPT_entry:
      if (!arg->getValue()[0])
        Fatal(ctx) << "missing entry point symbol name";
      ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
        symtab.entry = symtab.addGCRoot(symtab.mangle(arg->getValue()), true);
      });
      break;
    case OPT_failifmismatch:
      checkFailIfMismatch(arg->getValue(), file);
      break;
    case OPT_incl:
      file->symtab.addGCRoot(arg->getValue());
      break;
    case OPT_manifestdependency:
      ctx.config.manifestDependencies.insert(arg->getValue());
      break;
    case OPT_merge:
      parseMerge(arg->getValue());
      break;
    case OPT_nodefaultlib:
      ctx.config.noDefaultLibs.insert(findLib(arg->getValue()).lower());
      break;
    case OPT_release:
      ctx.config.writeCheckSum = true;
      break;
    case OPT_section:
      parseSection(arg->getValue());
      break;
    case OPT_stack:
      parseNumbers(arg->getValue(), &ctx.config.stackReserve,
                   &ctx.config.stackCommit);
      break;
    case OPT_subsystem: {
      bool gotVersion = false;
      parseSubsystem(arg->getValue(), &ctx.config.subsystem,
                     &ctx.config.majorSubsystemVersion,
                     &ctx.config.minorSubsystemVersion, &gotVersion);
      if (gotVersion) {
        ctx.config.majorOSVersion = ctx.config.majorSubsystemVersion;
        ctx.config.minorOSVersion = ctx.config.minorSubsystemVersion;
      }
      break;
    }
    // Only add flags here that link.exe accepts in
    // `#pragma comment(linker, "/flag")`-generated sections.
    case OPT_editandcontinue:
    case OPT_guardsym:
    case OPT_throwingnew:
    case OPT_inferasanlibs:
    case OPT_inferasanlibs_no:
      break;
    default:
      Err(ctx) << arg->getSpelling() << " is not allowed in .drectve ("
               << toString(file) << ")";
    }
  }
}

// Find file from search paths. You can omit ".obj", this function takes
// care of that. Note that the returned path is not guaranteed to exist.
StringRef LinkerDriver::findFile(StringRef filename) {
  auto getFilename = [this](StringRef filename) -> StringRef {
    if (ctx.config.vfs)
      if (auto statOrErr = ctx.config.vfs->status(filename))
        return saver().save(statOrErr->getName());
    return filename;
  };

  if (sys::path::is_absolute(filename))
    return getFilename(filename);
  bool hasExt = filename.contains('.');
  for (StringRef dir : searchPaths) {
    SmallString<128> path = dir;
    sys::path::append(path, filename);
    path = SmallString<128>{getFilename(path.str())};
    if (sys::fs::exists(path.str()))
      return saver().save(path.str());
    if (!hasExt) {
      path.append(".obj");
      path = SmallString<128>{getFilename(path.str())};
      if (sys::fs::exists(path.str()))
        return saver().save(path.str());
    }
  }
  return filename;
}

static std::optional<sys::fs::UniqueID> getUniqueID(StringRef path) {
  sys::fs::UniqueID ret;
  if (sys::fs::getUniqueID(path, ret))
    return std::nullopt;
  return ret;
}

// Resolves a file path. This never returns the same path
// (in that case, it returns std::nullopt).
std::optional<StringRef> LinkerDriver::findFileIfNew(StringRef filename) {
  StringRef path = findFile(filename);

  if (std::optional<sys::fs::UniqueID> id = getUniqueID(path)) {
    bool seen = !visitedFiles.insert(*id).second;
    if (seen)
      return std::nullopt;
  }

  if (path.ends_with_insensitive(".lib"))
    visitedLibs.insert(std::string(sys::path::filename(path).lower()));
  return path;
}

// MinGW specific. If an embedded directive specified to link to
// foo.lib, but it isn't found, try libfoo.a instead.
StringRef LinkerDriver::findLibMinGW(StringRef filename) {
  if (filename.contains('/') || filename.contains('\\'))
    return filename;

  SmallString<128> s = filename;
  sys::path::replace_extension(s, ".a");
  StringRef libName = saver().save("lib" + s.str());
  return findFile(libName);
}

// Find library file from search path.
StringRef LinkerDriver::findLib(StringRef filename) {
  // Add ".lib" to Filename if that has no file extension.
  bool hasExt = filename.contains('.');
  if (!hasExt)
    filename = saver().save(filename + ".lib");
  StringRef ret = findFile(filename);
  // For MinGW, if the find above didn't turn up anything, try
  // looking for a MinGW formatted library name.
  if (ctx.config.mingw && ret == filename)
    return findLibMinGW(filename);
  return ret;
}

// Resolves a library path. /nodefaultlib options are taken into
// consideration. This never returns the same path (in that case,
// it returns std::nullopt).
std::optional<StringRef> LinkerDriver::findLibIfNew(StringRef filename) {
  if (ctx.config.noDefaultLibAll)
    return std::nullopt;
  if (!visitedLibs.insert(filename.lower()).second)
    return std::nullopt;

  StringRef path = findLib(filename);
  if (ctx.config.noDefaultLibs.count(path.lower()))
    return std::nullopt;

  if (std::optional<sys::fs::UniqueID> id = getUniqueID(path))
    if (!visitedFiles.insert(*id).second)
      return std::nullopt;
  return path;
}

void LinkerDriver::setMachine(MachineTypes machine) {
  assert(ctx.config.machine == IMAGE_FILE_MACHINE_UNKNOWN);
  assert(machine != IMAGE_FILE_MACHINE_UNKNOWN);

  ctx.config.machine = machine;

  if (!isArm64EC(machine)) {
    ctx.symtab.machine = machine;
  } else {
    // Set up a hybrid symbol table on ARM64EC/ARM64X. This is primarily useful
    // on ARM64X, where both the native and EC symbol tables are meaningful.
    // However, since ARM64EC can include native object files, we also need to
    // support a hybrid symbol table there.
    ctx.symtab.machine = ARM64EC;
    ctx.hybridSymtab.emplace(ctx, ARM64);
  }

  addWinSysRootLibSearchPaths();
}

void LinkerDriver::detectWinSysRoot(const opt::InputArgList &Args) {
  IntrusiveRefCntPtr<vfs::FileSystem> VFS = vfs::getRealFileSystem();

  // Check the command line first, that's the user explicitly telling us what to
  // use. Check the environment next, in case we're being invoked from a VS
  // command prompt. Failing that, just try to find the newest Visual Studio
  // version we can and use its default VC toolchain.
  std::optional<StringRef> VCToolsDir, VCToolsVersion, WinSysRoot;
  if (auto *A = Args.getLastArg(OPT_vctoolsdir))
    VCToolsDir = A->getValue();
  if (auto *A = Args.getLastArg(OPT_vctoolsversion))
    VCToolsVersion = A->getValue();
  if (auto *A = Args.getLastArg(OPT_winsysroot))
    WinSysRoot = A->getValue();
  if (!findVCToolChainViaCommandLine(*VFS, VCToolsDir, VCToolsVersion,
                                     WinSysRoot, vcToolChainPath, vsLayout) &&
      (Args.hasArg(OPT_lldignoreenv) ||
       !findVCToolChainViaEnvironment(*VFS, vcToolChainPath, vsLayout)) &&
      !findVCToolChainViaSetupConfig(*VFS, {}, vcToolChainPath, vsLayout) &&
      !findVCToolChainViaRegistry(vcToolChainPath, vsLayout))
    return;

  // If the VC environment hasn't been configured (perhaps because the user did
  // not run vcvarsall), try to build a consistent link environment.  If the
  // environment variable is set however, assume the user knows what they're
  // doing. If the user passes /vctoolsdir or /winsdkdir, trust that over env
  // vars.
  if (const auto *A = Args.getLastArg(OPT_diasdkdir, OPT_winsysroot)) {
    diaPath = A->getValue();
    if (A->getOption().getID() == OPT_winsysroot)
      path::append(diaPath, "DIA SDK");
  }
  useWinSysRootLibPath = !Process::GetEnv("LIB") ||
                         Args.hasArg(OPT_lldignoreenv, OPT_vctoolsdir,
                                     OPT_vctoolsversion, OPT_winsysroot);
  if (!Process::GetEnv("LIB") ||
      Args.hasArg(OPT_lldignoreenv, OPT_winsdkdir, OPT_winsdkversion,
                  OPT_winsysroot)) {
    std::optional<StringRef> WinSdkDir, WinSdkVersion;
    if (auto *A = Args.getLastArg(OPT_winsdkdir))
      WinSdkDir = A->getValue();
    if (auto *A = Args.getLastArg(OPT_winsdkversion))
      WinSdkVersion = A->getValue();

    if (useUniversalCRT(vsLayout, vcToolChainPath, getArch(), *VFS)) {
      std::string UniversalCRTSdkPath;
      std::string UCRTVersion;
      if (getUniversalCRTSdkDir(*VFS, WinSdkDir, WinSdkVersion, WinSysRoot,
                                UniversalCRTSdkPath, UCRTVersion)) {
        universalCRTLibPath = UniversalCRTSdkPath;
        path::append(universalCRTLibPath, "Lib", UCRTVersion, "ucrt");
      }
    }

    std::string sdkPath;
    std::string windowsSDKIncludeVersion;
    std::string windowsSDKLibVersion;
    if (getWindowsSDKDir(*VFS, WinSdkDir, WinSdkVersion, WinSysRoot, sdkPath,
                         sdkMajor, windowsSDKIncludeVersion,
                         windowsSDKLibVersion)) {
      windowsSdkLibPath = sdkPath;
      path::append(windowsSdkLibPath, "Lib");
      if (sdkMajor >= 8)
        path::append(windowsSdkLibPath, windowsSDKLibVersion, "um");
    }
  }
}

void LinkerDriver::addClangLibSearchPaths(const std::string &argv0) {
  std::string lldBinary = sys::fs::getMainExecutable(argv0.c_str(), nullptr);
  SmallString<128> binDir(lldBinary);
  sys::path::remove_filename(binDir);                 // remove lld-link.exe
  StringRef rootDir = sys::path::parent_path(binDir); // remove 'bin'

  SmallString<128> libDir(rootDir);
  sys::path::append(libDir, "lib");

  // Add the resource dir library path
  SmallString<128> runtimeLibDir(rootDir);
  sys::path::append(runtimeLibDir, "lib", "clang",
                    std::to_string(LLVM_VERSION_MAJOR), "lib");
  // Resource dir + osname, which is hardcoded to windows since we are in the
  // COFF driver.
  SmallString<128> runtimeLibDirWithOS(runtimeLibDir);
  sys::path::append(runtimeLibDirWithOS, "windows");

  searchPaths.push_back(saver().save(runtimeLibDirWithOS.str()));
  searchPaths.push_back(saver().save(runtimeLibDir.str()));
  searchPaths.push_back(saver().save(libDir.str()));
}

void LinkerDriver::addWinSysRootLibSearchPaths() {
  if (!diaPath.empty()) {
    // The DIA SDK always uses the legacy vc arch, even in new MSVC versions.
    path::append(diaPath, "lib", archToLegacyVCArch(getArch()));
    searchPaths.push_back(saver().save(diaPath.str()));
  }
  if (useWinSysRootLibPath) {
    searchPaths.push_back(saver().save(getSubDirectoryPath(
        SubDirectoryType::Lib, vsLayout, vcToolChainPath, getArch())));
    searchPaths.push_back(saver().save(
        getSubDirectoryPath(SubDirectoryType::Lib, vsLayout, vcToolChainPath,
                            getArch(), "atlmfc")));
  }
  if (!universalCRTLibPath.empty()) {
    StringRef ArchName = archToWindowsSDKArch(getArch());
    if (!ArchName.empty()) {
      path::append(universalCRTLibPath, ArchName);
      searchPaths.push_back(saver().save(universalCRTLibPath.str()));
    }
  }
  if (!windowsSdkLibPath.empty()) {
    std::string path;
    if (appendArchToWindowsSDKLibPath(sdkMajor, windowsSdkLibPath, getArch(),
                                      path))
      searchPaths.push_back(saver().save(path));
  }

  // Libraries specified by `/nodefaultlib:` may not be found in incomplete
  // search paths before lld infers a machine type from input files.
  std::set<std::string> noDefaultLibs;
  for (const std::string &path : ctx.config.noDefaultLibs)
    noDefaultLibs.insert(findLib(path).lower());
  ctx.config.noDefaultLibs = noDefaultLibs;
}

// Parses LIB environment which contains a list of search paths.
void LinkerDriver::addLibSearchPaths() {
  std::optional<std::string> envOpt = Process::GetEnv("LIB");
  if (!envOpt)
    return;
  StringRef env = saver().save(*envOpt);
  while (!env.empty()) {
    StringRef path;
    std::tie(path, env) = env.split(';');
    searchPaths.push_back(path);
  }
}

uint64_t LinkerDriver::getDefaultImageBase() {
  if (ctx.config.is64())
    return ctx.config.dll ? 0x180000000 : 0x140000000;
  return ctx.config.dll ? 0x10000000 : 0x400000;
}

static std::string rewritePath(StringRef s) {
  if (fs::exists(s))
    return relativeToRoot(s);
  return std::string(s);
}

// Reconstructs command line arguments so that so that you can re-run
// the same command with the same inputs. This is for --reproduce.
static std::string createResponseFile(const opt::InputArgList &args,
                                      ArrayRef<StringRef> searchPaths) {
  SmallString<0> data;
  raw_svector_ostream os(data);

  for (auto *arg : args) {
    switch (arg->getOption().getID()) {
    case OPT_linkrepro:
    case OPT_reproduce:
    case OPT_libpath:
    case OPT_winsysroot:
      break;
    case OPT_INPUT:
      os << quote(rewritePath(arg->getValue())) << "\n";
      break;
    case OPT_wholearchive_file:
      os << arg->getSpelling() << quote(rewritePath(arg->getValue())) << "\n";
      break;
    case OPT_call_graph_ordering_file:
    case OPT_deffile:
    case OPT_manifestinput:
    case OPT_natvis:
      os << arg->getSpelling() << quote(rewritePath(arg->getValue())) << '\n';
      break;
    case OPT_order: {
      StringRef orderFile = arg->getValue();
      orderFile.consume_front("@");
      os << arg->getSpelling() << '@' << quote(rewritePath(orderFile)) << '\n';
      break;
    }
    case OPT_pdbstream: {
      const std::pair<StringRef, StringRef> nameFile =
          StringRef(arg->getValue()).split("=");
      os << arg->getSpelling() << nameFile.first << '='
         << quote(rewritePath(nameFile.second)) << '\n';
      break;
    }
    case OPT_implib:
    case OPT_manifestfile:
    case OPT_pdb:
    case OPT_pdbstripped:
    case OPT_out:
      os << arg->getSpelling() << sys::path::filename(arg->getValue()) << "\n";
      break;
    default:
      os << toString(*arg) << "\n";
    }
  }

  for (StringRef path : searchPaths) {
    std::string relPath = relativeToRoot(path);
    os << "/libpath:" << quote(relPath) << "\n";
  }

  return std::string(data);
}

static unsigned parseDebugTypes(COFFLinkerContext &ctx,
                                const opt::InputArgList &args) {
  unsigned debugTypes = static_cast<unsigned>(DebugType::None);

  if (auto *a = args.getLastArg(OPT_debugtype)) {
    SmallVector<StringRef, 3> types;
    StringRef(a->getValue())
        .split(types, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);

    for (StringRef type : types) {
      unsigned v = StringSwitch<unsigned>(type.lower())
                       .Case("cv", static_cast<unsigned>(DebugType::CV))
                       .Case("pdata", static_cast<unsigned>(DebugType::PData))
                       .Case("fixup", static_cast<unsigned>(DebugType::Fixup))
                       .Default(0);
      if (v == 0) {
        Warn(ctx) << "/debugtype: unknown option '" << type << "'";
        continue;
      }
      debugTypes |= v;
    }
    return debugTypes;
  }

  // Default debug types
  debugTypes = static_cast<unsigned>(DebugType::CV);
  if (args.hasArg(OPT_driver))
    debugTypes |= static_cast<unsigned>(DebugType::PData);
  if (args.hasArg(OPT_profile))
    debugTypes |= static_cast<unsigned>(DebugType::Fixup);

  return debugTypes;
}

std::string LinkerDriver::getMapFile(const opt::InputArgList &args,
                                     opt::OptSpecifier os,
                                     opt::OptSpecifier osFile) {
  auto *arg = args.getLastArg(os, osFile);
  if (!arg)
    return "";
  if (arg->getOption().getID() == osFile.getID())
    return arg->getValue();

  assert(arg->getOption().getID() == os.getID());
  StringRef outFile = ctx.config.outputFile;
  return (outFile.substr(0, outFile.rfind('.')) + ".map").str();
}

std::string LinkerDriver::getImplibPath() {
  if (!ctx.config.implib.empty())
    return std::string(ctx.config.implib);
  SmallString<128> out = StringRef(ctx.config.outputFile);
  sys::path::replace_extension(out, ".lib");
  return std::string(out);
}

// The import name is calculated as follows:
//
//        | LIBRARY w/ ext |   LIBRARY w/o ext   | no LIBRARY
//   -----+----------------+---------------------+------------------
//   LINK | {value}        | {value}.{.dll/.exe} | {output name}
//    LIB | {value}        | {value}.dll         | {output name}.dll
//
std::string LinkerDriver::getImportName(bool asLib) {
  SmallString<128> out;

  if (ctx.config.importName.empty()) {
    out.assign(sys::path::filename(ctx.config.outputFile));
    if (asLib)
      sys::path::replace_extension(out, ".dll");
  } else {
    out.assign(ctx.config.importName);
    if (!sys::path::has_extension(out))
      sys::path::replace_extension(out,
                                   (ctx.config.dll || asLib) ? ".dll" : ".exe");
  }

  return std::string(out);
}

void LinkerDriver::createImportLibrary(bool asLib) {
  llvm::TimeTraceScope timeScope("Create import library");
  std::vector<COFFShortExport> exports, nativeExports;

  auto getExports = [](SymbolTable &symtab,
                       std::vector<COFFShortExport> &exports) {
    for (Export &e1 : symtab.exports) {
      COFFShortExport e2;
      e2.Name = std::string(e1.name);
      e2.SymbolName = std::string(e1.symbolName);
      e2.ExtName = std::string(e1.extName);
      e2.ExportAs = std::string(e1.exportAs);
      e2.ImportName = std::string(e1.importName);
      e2.Ordinal = e1.ordinal;
      e2.Noname = e1.noname;
      e2.Data = e1.data;
      e2.Private = e1.isPrivate;
      e2.Constant = e1.constant;
      exports.push_back(e2);
    }
  };

  getExports(ctx.symtab, exports);
  if (ctx.config.machine == ARM64X)
    getExports(*ctx.hybridSymtab, nativeExports);

  std::string libName = getImportName(asLib);
  std::string path = getImplibPath();

  if (!ctx.config.incremental) {
    checkError(writeImportLibrary(libName, path, exports, ctx.config.machine,
                                  ctx.config.mingw, nativeExports));
    return;
  }

  // If the import library already exists, replace it only if the contents
  // have changed.
  ErrorOr<std::unique_ptr<MemoryBuffer>> oldBuf = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!oldBuf) {
    checkError(writeImportLibrary(libName, path, exports, ctx.config.machine,
                                  ctx.config.mingw, nativeExports));
    return;
  }

  SmallString<128> tmpName;
  if (std::error_code ec =
          sys::fs::createUniqueFile(path + ".tmp-%%%%%%%%.lib", tmpName))
    Fatal(ctx) << "cannot create temporary file for import library " << path
               << ": " << ec.message();

  if (Error e =
          writeImportLibrary(libName, tmpName, exports, ctx.config.machine,
                             ctx.config.mingw, nativeExports)) {
    checkError(std::move(e));
    return;
  }

  std::unique_ptr<MemoryBuffer> newBuf = check(MemoryBuffer::getFile(
      tmpName, /*IsText=*/false, /*RequiresNullTerminator=*/false));
  if ((*oldBuf)->getBuffer() != newBuf->getBuffer()) {
    oldBuf->reset();
    checkError(errorCodeToError(sys::fs::rename(tmpName, path)));
  } else {
    sys::fs::remove(tmpName);
  }
}

void LinkerDriver::enqueueTask(std::function<void()> task) {
  taskQueue.push_back(std::move(task));
}

bool LinkerDriver::run() {
  llvm::TimeTraceScope timeScope("Read input files");
  ScopedTimer t(ctx.inputFileTimer);

  bool didWork = !taskQueue.empty();
  while (!taskQueue.empty()) {
    taskQueue.front()();
    taskQueue.pop_front();
  }
  return didWork;
}

// Parse an /order file. If an option is given, the linker places
// COMDAT sections in the same order as their names appear in the
// given file.
void LinkerDriver::parseOrderFile(StringRef arg) {
  // For some reason, the MSVC linker requires a filename to be
  // preceded by "@".
  if (!arg.starts_with("@")) {
    Err(ctx) << "malformed /order option: '@' missing";
    return;
  }

  // Get a list of all comdat sections for error checking.
  DenseSet<StringRef> set;
  for (Chunk *c : ctx.driver.getChunks())
    if (auto *sec = dyn_cast<SectionChunk>(c))
      if (sec->sym)
        set.insert(sec->sym->getName());

  // Open a file.
  StringRef path = arg.substr(1);
  std::unique_ptr<MemoryBuffer> mb =
      CHECK(MemoryBuffer::getFile(path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false,
                                  /*IsVolatile=*/true),
            "could not open " + path);

  // Parse a file. An order file contains one symbol per line.
  // All symbols that were not present in a given order file are
  // considered to have the lowest priority 0 and are placed at
  // end of an output section.
  for (StringRef arg : args::getLines(mb->getMemBufferRef())) {
    std::string s(arg);
    if (ctx.config.machine == I386 && !isDecorated(s))
      s = "_" + s;

    if (set.count(s) == 0) {
      if (ctx.config.warnMissingOrderSymbol)
        Warn(ctx) << "/order:" << arg << ": missing symbol: " << s
                  << " [LNK4037]";
    } else
      ctx.config.order[s] = INT_MIN + ctx.config.order.size();
  }

  // Include in /reproduce: output if applicable.
  ctx.driver.takeBuffer(std::move(mb));
}

void LinkerDriver::parseCallGraphFile(StringRef path) {
  std::unique_ptr<MemoryBuffer> mb =
      CHECK(MemoryBuffer::getFile(path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false,
                                  /*IsVolatile=*/true),
            "could not open " + path);

  // Build a map from symbol name to section.
  DenseMap<StringRef, Symbol *> map;
  for (ObjFile *file : ctx.objFileInstances)
    for (Symbol *sym : file->getSymbols())
      if (sym)
        map[sym->getName()] = sym;

  auto findSection = [&](StringRef name) -> SectionChunk * {
    Symbol *sym = map.lookup(name);
    if (!sym) {
      if (ctx.config.warnMissingOrderSymbol)
        Warn(ctx) << path << ": no such symbol: " << name;
      return nullptr;
    }

    if (DefinedCOFF *dr = dyn_cast_or_null<DefinedCOFF>(sym))
      return dyn_cast_or_null<SectionChunk>(dr->getChunk());
    return nullptr;
  };

  for (StringRef line : args::getLines(*mb)) {
    SmallVector<StringRef, 3> fields;
    line.split(fields, ' ');
    uint64_t count;

    if (fields.size() != 3 || !to_integer(fields[2], count)) {
      Err(ctx) << path << ": parse error";
      return;
    }

    if (SectionChunk *from = findSection(fields[0]))
      if (SectionChunk *to = findSection(fields[1]))
        ctx.config.callGraphProfile[{from, to}] += count;
  }

  // Include in /reproduce: output if applicable.
  ctx.driver.takeBuffer(std::move(mb));
}

static void readCallGraphsFromObjectFiles(COFFLinkerContext &ctx) {
  for (ObjFile *obj : ctx.objFileInstances) {
    if (obj->callgraphSec) {
      ArrayRef<uint8_t> contents;
      cantFail(
          obj->getCOFFObj()->getSectionContents(obj->callgraphSec, contents));
      BinaryStreamReader reader(contents, llvm::endianness::little);
      while (!reader.empty()) {
        uint32_t fromIndex, toIndex;
        uint64_t count;
        if (Error err = reader.readInteger(fromIndex))
          Fatal(ctx) << toString(obj) << ": Expected 32-bit integer";
        if (Error err = reader.readInteger(toIndex))
          Fatal(ctx) << toString(obj) << ": Expected 32-bit integer";
        if (Error err = reader.readInteger(count))
          Fatal(ctx) << toString(obj) << ": Expected 64-bit integer";
        auto *fromSym = dyn_cast_or_null<Defined>(obj->getSymbol(fromIndex));
        auto *toSym = dyn_cast_or_null<Defined>(obj->getSymbol(toIndex));
        if (!fromSym || !toSym)
          continue;
        auto *from = dyn_cast_or_null<SectionChunk>(fromSym->getChunk());
        auto *to = dyn_cast_or_null<SectionChunk>(toSym->getChunk());
        if (from && to)
          ctx.config.callGraphProfile[{from, to}] += count;
      }
    }
  }
}

static void markAddrsig(Symbol *s) {
  if (auto *d = dyn_cast_or_null<Defined>(s))
    if (SectionChunk *c = dyn_cast_or_null<SectionChunk>(d->getChunk()))
      c->keepUnique = true;
}

static void findKeepUniqueSections(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Find keep unique sections");

  // Exported symbols could be address-significant in other executables or DSOs,
  // so we conservatively mark them as address-significant.
  ctx.forEachSymtab([](SymbolTable &symtab) {
    for (Export &r : symtab.exports)
      markAddrsig(r.sym);
  });

  // Visit the address-significance table in each object file and mark each
  // referenced symbol as address-significant.
  for (ObjFile *obj : ctx.objFileInstances) {
    ArrayRef<Symbol *> syms = obj->getSymbols();
    if (obj->addrsigSec) {
      ArrayRef<uint8_t> contents;
      cantFail(
          obj->getCOFFObj()->getSectionContents(obj->addrsigSec, contents));
      const uint8_t *cur = contents.begin();
      while (cur != contents.end()) {
        unsigned size;
        const char *err = nullptr;
        uint64_t symIndex = decodeULEB128(cur, &size, contents.end(), &err);
        if (err)
          Fatal(ctx) << toString(obj)
                     << ": could not decode addrsig section: " << err;
        if (symIndex >= syms.size())
          Fatal(ctx) << toString(obj)
                     << ": invalid symbol index in addrsig section";
        markAddrsig(syms[symIndex]);
        cur += size;
      }
    } else {
      // If an object file does not have an address-significance table,
      // conservatively mark all of its symbols as address-significant.
      for (Symbol *s : syms)
        markAddrsig(s);
    }
  }
}

// link.exe replaces each %foo% in altPath with the contents of environment
// variable foo, and adds the two magic env vars _PDB (expands to the basename
// of pdb's output path) and _EXT (expands to the extension of the output
// binary).
// lld only supports %_PDB% and %_EXT% and warns on references to all other env
// vars.
void LinkerDriver::parsePDBAltPath() {
  SmallString<128> buf;
  StringRef pdbBasename =
      sys::path::filename(ctx.config.pdbPath, sys::path::Style::windows);
  StringRef binaryExtension =
      sys::path::extension(ctx.config.outputFile, sys::path::Style::windows);
  if (!binaryExtension.empty())
    binaryExtension = binaryExtension.substr(1); // %_EXT% does not include '.'.

  // Invariant:
  //   +--------- cursor ('a...' might be the empty string).
  //   |   +----- firstMark
  //   |   |   +- secondMark
  //   v   v   v
  //   a...%...%...
  size_t cursor = 0;
  while (cursor < ctx.config.pdbAltPath.size()) {
    size_t firstMark, secondMark;
    if ((firstMark = ctx.config.pdbAltPath.find('%', cursor)) ==
            StringRef::npos ||
        (secondMark = ctx.config.pdbAltPath.find('%', firstMark + 1)) ==
            StringRef::npos) {
      // Didn't find another full fragment, treat rest of string as literal.
      buf.append(ctx.config.pdbAltPath.substr(cursor));
      break;
    }

    // Found a full fragment. Append text in front of first %, and interpret
    // text between first and second % as variable name.
    buf.append(ctx.config.pdbAltPath.substr(cursor, firstMark - cursor));
    StringRef var =
        ctx.config.pdbAltPath.substr(firstMark, secondMark - firstMark + 1);
    if (var.equals_insensitive("%_pdb%"))
      buf.append(pdbBasename);
    else if (var.equals_insensitive("%_ext%"))
      buf.append(binaryExtension);
    else {
      Warn(ctx) << "only %_PDB% and %_EXT% supported in /pdbaltpath:, keeping "
                << var << " as literal";
      buf.append(var);
    }

    cursor = secondMark + 1;
  }

  ctx.config.pdbAltPath = buf;
}

/// Convert resource files and potentially merge input resource object
/// trees into one resource tree.
/// Call after ObjFile::Instances is complete.
void LinkerDriver::convertResources() {
  llvm::TimeTraceScope timeScope("Convert resources");
  std::vector<ObjFile *> resourceObjFiles;

  for (ObjFile *f : ctx.objFileInstances) {
    if (f->isResourceObjFile())
      resourceObjFiles.push_back(f);
  }

  if (!ctx.config.mingw &&
      (resourceObjFiles.size() > 1 ||
       (resourceObjFiles.size() == 1 && !resources.empty()))) {
    Err(ctx) << (!resources.empty()
                     ? "internal .obj file created from .res files"
                     : toString(resourceObjFiles[1]))
             << ": more than one resource obj file not allowed, already got "
             << resourceObjFiles.front();
    return;
  }

  if (resources.empty() && resourceObjFiles.size() <= 1) {
    // No resources to convert, and max one resource object file in
    // the input. Keep that preconverted resource section as is.
    for (ObjFile *f : resourceObjFiles)
      f->includeResourceChunks();
    return;
  }
  ObjFile *f =
      ObjFile::create(ctx, convertResToCOFF(resources, resourceObjFiles));
  addFile(f);
  f->includeResourceChunks();
}

void LinkerDriver::maybeCreateECExportThunk(StringRef name, Symbol *&sym) {
  Defined *def;
  if (!sym)
    return;
  if (auto undef = dyn_cast<Undefined>(sym))
    def = undef->getDefinedWeakAlias();
  else
    def = dyn_cast<Defined>(sym);
  if (!def)
    return;

  if (def->getChunk()->getArm64ECRangeType() != chpe_range_type::Arm64EC)
    return;
  StringRef expName;
  if (auto mangledName = getArm64ECMangledFunctionName(name))
    expName = saver().save("EXP+" + *mangledName);
  else
    expName = saver().save("EXP+" + name);
  sym = ctx.symtab.addGCRoot(expName);
  if (auto undef = dyn_cast<Undefined>(sym)) {
    if (!undef->getWeakAlias()) {
      auto thunk = make<ECExportThunkChunk>(def);
      replaceSymbol<DefinedSynthetic>(undef, undef->getName(), thunk);
    }
  }
}

void LinkerDriver::createECExportThunks() {
  // Check if EXP+ symbols have corresponding $hp_target symbols and use them
  // to create export thunks when available.
  for (Symbol *s : ctx.symtab.expSymbols) {
    if (!s->isUsedInRegularObj)
      continue;
    assert(s->getName().starts_with("EXP+"));
    std::string targetName =
        (s->getName().substr(strlen("EXP+")) + "$hp_target").str();
    Symbol *sym = ctx.symtab.find(targetName);
    if (!sym)
      continue;
    Defined *targetSym;
    if (auto undef = dyn_cast<Undefined>(sym))
      targetSym = undef->getDefinedWeakAlias();
    else
      targetSym = dyn_cast<Defined>(sym);
    if (!targetSym)
      continue;

    auto *undef = dyn_cast<Undefined>(s);
    if (undef && !undef->getWeakAlias()) {
      auto thunk = make<ECExportThunkChunk>(targetSym);
      replaceSymbol<DefinedSynthetic>(undef, undef->getName(), thunk);
    }
    if (!targetSym->isGCRoot) {
      targetSym->isGCRoot = true;
      ctx.config.gcroot.push_back(targetSym);
    }
  }

  if (ctx.symtab.entry)
    maybeCreateECExportThunk(ctx.symtab.entry->getName(), ctx.symtab.entry);
  for (Export &e : ctx.symtab.exports) {
    if (!e.data)
      maybeCreateECExportThunk(e.extName.empty() ? e.name : e.extName, e.sym);
  }
}

void LinkerDriver::pullArm64ECIcallHelper() {
  if (!ctx.config.arm64ECIcallHelper)
    ctx.config.arm64ECIcallHelper =
        ctx.symtab.addGCRoot("__icall_helper_arm64ec");
}

// In MinGW, if no symbols are chosen to be exported, then all symbols are
// automatically exported by default. This behavior can be forced by the
// -export-all-symbols option, so that it happens even when exports are
// explicitly specified. The automatic behavior can be disabled using the
// -exclude-all-symbols option, so that lld-link behaves like link.exe rather
// than MinGW in the case that nothing is explicitly exported.
void LinkerDriver::maybeExportMinGWSymbols(const opt::InputArgList &args) {
  if (!args.hasArg(OPT_export_all_symbols)) {
    if (!ctx.config.dll)
      return;

    if (ctx.symtab.hadExplicitExports ||
        (ctx.config.machine == ARM64X && ctx.hybridSymtab->hadExplicitExports))
      return;
    if (args.hasArg(OPT_exclude_all_symbols))
      return;
  }

  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    AutoExporter exporter(symtab, excludedSymbols);

    for (auto *arg : args.filtered(OPT_wholearchive_file))
      if (std::optional<StringRef> path = findFile(arg->getValue()))
        exporter.addWholeArchive(*path);

    for (auto *arg : args.filtered(OPT_exclude_symbols)) {
      SmallVector<StringRef, 2> vec;
      StringRef(arg->getValue()).split(vec, ',');
      for (StringRef sym : vec)
        exporter.addExcludedSymbol(symtab.mangle(sym));
    }

    symtab.forEachSymbol([&](Symbol *s) {
      auto *def = dyn_cast<Defined>(s);
      if (!exporter.shouldExport(def))
        return;

      if (!def->isGCRoot) {
        def->isGCRoot = true;
        ctx.config.gcroot.push_back(def);
      }

      Export e;
      e.name = def->getName();
      e.sym = def;
      if (Chunk *c = def->getChunk())
        if (!(c->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
          e.data = true;
      s->isUsedInRegularObj = true;
      symtab.exports.push_back(e);
    });
  });
}

// lld has a feature to create a tar file containing all input files as well as
// all command line options, so that other people can run lld again with exactly
// the same inputs. This feature is accessible via /linkrepro and /reproduce.
//
// /linkrepro and /reproduce are very similar, but /linkrepro takes a directory
// name while /reproduce takes a full path. We have /linkrepro for compatibility
// with Microsoft link.exe.
std::optional<std::string> getReproduceFile(const opt::InputArgList &args) {
  if (auto *arg = args.getLastArg(OPT_reproduce))
    return std::string(arg->getValue());

  if (auto *arg = args.getLastArg(OPT_linkrepro)) {
    SmallString<64> path = StringRef(arg->getValue());
    sys::path::append(path, "repro.tar");
    return std::string(path);
  }

  // This is intentionally not guarded by OPT_lldignoreenv since writing
  // a repro tar file doesn't affect the main output.
  if (auto *path = getenv("LLD_REPRODUCE"))
    return std::string(path);

  return std::nullopt;
}

static std::unique_ptr<llvm::vfs::FileSystem>
getVFS(COFFLinkerContext &ctx, const opt::InputArgList &args) {
  using namespace llvm::vfs;

  const opt::Arg *arg = args.getLastArg(OPT_vfsoverlay);
  if (!arg)
    return nullptr;

  auto bufOrErr = llvm::MemoryBuffer::getFile(arg->getValue());
  if (!bufOrErr) {
    checkError(errorCodeToError(bufOrErr.getError()));
    return nullptr;
  }

  if (auto ret = vfs::getVFSFromYAML(std::move(*bufOrErr),
                                     /*DiagHandler*/ nullptr, arg->getValue()))
    return ret;

  Err(ctx) << "Invalid vfs overlay";
  return nullptr;
}

constexpr const char *lldsaveTempsValues[] = {
    "resolution", "preopt",     "promote", "internalize",  "import",
    "opt",        "precodegen", "prelink", "combinedindex"};

void LinkerDriver::linkerMain(ArrayRef<const char *> argsArr) {
  ScopedTimer rootTimer(ctx.rootTimer);
  Configuration *config = &ctx.config;

  // Needed for LTO.
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllAsmPrinters();

  // If the first command line argument is "/lib", link.exe acts like lib.exe.
  // We call our own implementation of lib.exe that understands bitcode files.
  if (argsArr.size() > 1 &&
      (StringRef(argsArr[1]).equals_insensitive("/lib") ||
       StringRef(argsArr[1]).equals_insensitive("-lib"))) {
    if (llvm::libDriverMain(argsArr.slice(1)) != 0)
      Fatal(ctx) << "lib failed";
    return;
  }

  // Parse command line options.
  ArgParser parser(ctx);
  opt::InputArgList args = parser.parse(argsArr);

  // Initialize time trace profiler.
  config->timeTraceEnabled = args.hasArg(OPT_time_trace_eq);
  config->timeTraceGranularity =
      args::getInteger(args, OPT_time_trace_granularity_eq, 500);

  if (config->timeTraceEnabled)
    timeTraceProfilerInitialize(config->timeTraceGranularity, argsArr[0]);

  llvm::TimeTraceScope timeScope("COFF link");

  // Parse and evaluate -mllvm options.
  std::vector<const char *> v;
  v.push_back("lld-link (LLVM option parsing)");
  for (const auto *arg : args.filtered(OPT_mllvm)) {
    v.push_back(arg->getValue());
    config->mllvmOpts.emplace_back(arg->getValue());
  }
  {
    llvm::TimeTraceScope timeScope2("Parse cl::opt");
    cl::ResetAllOptionOccurrences();
    cl::ParseCommandLineOptions(v.size(), v.data());
  }

  // Handle /errorlimit early, because error() depends on it.
  if (auto *arg = args.getLastArg(OPT_errorlimit)) {
    int n = 20;
    StringRef s = arg->getValue();
    if (s.getAsInteger(10, n))
      Err(ctx) << arg->getSpelling() << " number expected, but got " << s;
    ctx.e.errorLimit = n;
  }

  config->vfs = getVFS(ctx, args);

  // Handle /help
  if (args.hasArg(OPT_help)) {
    printHelp(argsArr[0]);
    return;
  }

  // /threads: takes a positive integer and provides the default value for
  // /opt:lldltojobs=.
  if (auto *arg = args.getLastArg(OPT_threads)) {
    StringRef v(arg->getValue());
    unsigned threads = 0;
    if (!llvm::to_integer(v, threads, 0) || threads == 0)
      Err(ctx) << arg->getSpelling()
               << ": expected a positive integer, but got '" << arg->getValue()
               << "'";
    parallel::strategy = hardware_concurrency(threads);
    config->thinLTOJobs = v.str();
  }

  if (args.hasArg(OPT_show_timing))
    config->showTiming = true;

  config->showSummary = args.hasArg(OPT_summary);
  config->printSearchPaths = args.hasArg(OPT_print_search_paths);

  // Handle --version, which is an lld extension. This option is a bit odd
  // because it doesn't start with "/", but we deliberately chose "--" to
  // avoid conflict with /version and for compatibility with clang-cl.
  if (args.hasArg(OPT_dash_dash_version)) {
    Msg(ctx) << getLLDVersion();
    return;
  }

  // Handle /lldmingw early, since it can potentially affect how other
  // options are handled.
  config->mingw = args.hasArg(OPT_lldmingw);
  if (config->mingw)
    ctx.e.errorLimitExceededMsg = "too many errors emitted, stopping now"
                                  " (use --error-limit=0 to see all errors)";

  // Handle /linkrepro and /reproduce.
  {
    llvm::TimeTraceScope timeScope2("Reproducer");
    if (std::optional<std::string> path = getReproduceFile(args)) {
      Expected<std::unique_ptr<TarWriter>> errOrWriter =
          TarWriter::create(*path, sys::path::stem(*path));

      if (errOrWriter) {
        tar = std::move(*errOrWriter);
      } else {
        Err(ctx) << "/linkrepro: failed to open " << *path << ": "
                 << toString(errOrWriter.takeError());
      }
    }
  }

  if (!args.hasArg(OPT_INPUT, OPT_wholearchive_file)) {
    if (args.hasArg(OPT_deffile))
      config->noEntry = true;
    else
      Fatal(ctx) << "no input files";
  }

  // Construct search path list.
  {
    llvm::TimeTraceScope timeScope2("Search paths");
    searchPaths.emplace_back("");
    for (auto *arg : args.filtered(OPT_libpath))
      searchPaths.push_back(arg->getValue());
    if (!config->mingw) {
      // Prefer the Clang provided builtins over the ones bundled with MSVC.
      // In MinGW mode, the compiler driver passes the necessary libpath
      // options explicitly.
      addClangLibSearchPaths(argsArr[0]);
      // Don't automatically deduce the lib path from the environment or MSVC
      // installations when operating in mingw mode. (This also makes LLD ignore
      // winsysroot and vctoolsdir arguments.)
      detectWinSysRoot(args);
      if (!args.hasArg(OPT_lldignoreenv, OPT_winsysroot, OPT_vctoolsdir,
                       OPT_vctoolsversion, OPT_winsdkdir, OPT_winsdkversion))
        addLibSearchPaths();
    } else {
      if (args.hasArg(OPT_vctoolsdir, OPT_winsysroot))
        Warn(ctx) << "ignoring /vctoolsdir or /winsysroot flags in MinGW mode";
    }
  }

  // Handle /ignore
  for (auto *arg : args.filtered(OPT_ignore)) {
    SmallVector<StringRef, 8> vec;
    StringRef(arg->getValue()).split(vec, ',');
    for (StringRef s : vec) {
      if (s == "4037")
        config->warnMissingOrderSymbol = false;
      else if (s == "4099")
        config->warnDebugInfoUnusable = false;
      else if (s == "4217")
        config->warnLocallyDefinedImported = false;
      else if (s == "longsections")
        config->warnLongSectionNames = false;
      else if (s == "importeddllmain")
        config->warnImportedDllMain = false;
      // Other warning numbers are ignored.
    }
  }

  // Handle /out
  if (auto *arg = args.getLastArg(OPT_out))
    config->outputFile = arg->getValue();

  // Handle /verbose
  if (args.hasArg(OPT_verbose))
    config->verbose = true;
  ctx.e.verbose = config->verbose;

  // Handle /force or /force:unresolved
  if (args.hasArg(OPT_force, OPT_force_unresolved))
    config->forceUnresolved = true;

  // Handle /force or /force:multiple
  if (args.hasArg(OPT_force, OPT_force_multiple))
    config->forceMultiple = true;

  // Handle /force or /force:multipleres
  if (args.hasArg(OPT_force, OPT_force_multipleres))
    config->forceMultipleRes = true;

  // Don't warn about long section names, such as .debug_info, for mingw (or
  // when -debug:dwarf is requested, handled below).
  if (config->mingw)
    config->warnLongSectionNames = false;

  bool doGC = true;

  // Handle /debug
  bool shouldCreatePDB = false;
  for (auto *arg : args.filtered(OPT_debug, OPT_debug_opt)) {
    std::string str;
    if (arg->getOption().getID() == OPT_debug)
      str = "full";
    else
      str = StringRef(arg->getValue()).lower();
    SmallVector<StringRef, 1> vec;
    StringRef(str).split(vec, ',');
    for (StringRef s : vec) {
      if (s == "fastlink") {
        Warn(ctx) << "/debug:fastlink unsupported; using /debug:full";
        s = "full";
      }
      if (s == "none") {
        config->debug = false;
        config->incremental = false;
        config->includeDwarfChunks = false;
        config->debugGHashes = false;
        config->writeSymtab = false;
        shouldCreatePDB = false;
        doGC = true;
      } else if (s == "full" || s == "ghash" || s == "noghash") {
        config->debug = true;
        config->incremental = true;
        config->includeDwarfChunks = true;
        if (s == "full" || s == "ghash")
          config->debugGHashes = true;
        shouldCreatePDB = true;
        doGC = false;
      } else if (s == "dwarf") {
        config->debug = true;
        config->incremental = true;
        config->includeDwarfChunks = true;
        config->writeSymtab = true;
        config->warnLongSectionNames = false;
        doGC = false;
      } else if (s == "nodwarf") {
        config->includeDwarfChunks = false;
      } else if (s == "symtab") {
        config->writeSymtab = true;
        doGC = false;
      } else if (s == "nosymtab") {
        config->writeSymtab = false;
      } else {
        Err(ctx) << "/debug: unknown option: " << s;
      }
    }
  }

  // Handle /demangle
  config->demangle = args.hasFlag(OPT_demangle, OPT_demangle_no, true);

  // Handle /debugtype
  config->debugTypes = parseDebugTypes(ctx, args);

  // Handle /driver[:uponly|:wdm].
  config->driverUponly = args.hasArg(OPT_driver_uponly) ||
                         args.hasArg(OPT_driver_uponly_wdm) ||
                         args.hasArg(OPT_driver_wdm_uponly);
  config->driverWdm = args.hasArg(OPT_driver_wdm) ||
                      args.hasArg(OPT_driver_uponly_wdm) ||
                      args.hasArg(OPT_driver_wdm_uponly);
  config->driver =
      config->driverUponly || config->driverWdm || args.hasArg(OPT_driver);

  // Handle /pdb
  if (shouldCreatePDB) {
    if (auto *arg = args.getLastArg(OPT_pdb))
      config->pdbPath = arg->getValue();
    if (auto *arg = args.getLastArg(OPT_pdbaltpath))
      config->pdbAltPath = arg->getValue();
    if (auto *arg = args.getLastArg(OPT_pdbpagesize))
      parsePDBPageSize(arg->getValue());
    if (args.hasArg(OPT_natvis))
      config->natvisFiles = args.getAllArgValues(OPT_natvis);
    if (args.hasArg(OPT_pdbstream)) {
      for (const StringRef value : args.getAllArgValues(OPT_pdbstream)) {
        const std::pair<StringRef, StringRef> nameFile = value.split("=");
        const StringRef name = nameFile.first;
        const std::string file = nameFile.second.str();
        config->namedStreams[name] = file;
      }
    }

    if (auto *arg = args.getLastArg(OPT_pdb_source_path))
      config->pdbSourcePath = arg->getValue();
  }

  // Handle /pdbstripped
  if (args.hasArg(OPT_pdbstripped))
    Warn(ctx) << "ignoring /pdbstripped flag, it is not yet supported";

  // Handle /noentry
  if (args.hasArg(OPT_noentry)) {
    if (args.hasArg(OPT_dll))
      config->noEntry = true;
    else
      Err(ctx) << "/noentry must be specified with /dll";
  }

  // Handle /dll
  if (args.hasArg(OPT_dll)) {
    config->dll = true;
    config->manifestID = 2;
  }

  // Handle /dynamicbase and /fixed. We can't use hasFlag for /dynamicbase
  // because we need to explicitly check whether that option or its inverse was
  // present in the argument list in order to handle /fixed.
  auto *dynamicBaseArg = args.getLastArg(OPT_dynamicbase, OPT_dynamicbase_no);
  if (dynamicBaseArg &&
      dynamicBaseArg->getOption().getID() == OPT_dynamicbase_no)
    config->dynamicBase = false;

  // MSDN claims "/FIXED:NO is the default setting for a DLL, and /FIXED is the
  // default setting for any other project type.", but link.exe defaults to
  // /FIXED:NO for exe outputs as well. Match behavior, not docs.
  bool fixed = args.hasFlag(OPT_fixed, OPT_fixed_no, false);
  if (fixed) {
    if (dynamicBaseArg &&
        dynamicBaseArg->getOption().getID() == OPT_dynamicbase) {
      Err(ctx) << "/fixed must not be specified with /dynamicbase";
    } else {
      config->relocatable = false;
      config->dynamicBase = false;
    }
  }

  // Handle /appcontainer
  config->appContainer =
      args.hasFlag(OPT_appcontainer, OPT_appcontainer_no, false);

  // Handle /machine
  {
    llvm::TimeTraceScope timeScope2("Machine arg");
    if (auto *arg = args.getLastArg(OPT_machine)) {
      MachineTypes machine = getMachineType(arg->getValue());
      if (machine == IMAGE_FILE_MACHINE_UNKNOWN)
        Fatal(ctx) << "unknown /machine argument: " << arg->getValue();
      setMachine(machine);
    }
  }

  // Handle /nodefaultlib:<filename>
  {
    llvm::TimeTraceScope timeScope2("Nodefaultlib");
    for (auto *arg : args.filtered(OPT_nodefaultlib))
      config->noDefaultLibs.insert(findLib(arg->getValue()).lower());
  }

  // Handle /nodefaultlib
  if (args.hasArg(OPT_nodefaultlib_all))
    config->noDefaultLibAll = true;

  // Handle /base
  if (auto *arg = args.getLastArg(OPT_base))
    parseNumbers(arg->getValue(), &config->imageBase);

  // Handle /filealign
  if (auto *arg = args.getLastArg(OPT_filealign)) {
    parseNumbers(arg->getValue(), &config->fileAlign);
    if (!isPowerOf2_64(config->fileAlign))
      Err(ctx) << "/filealign: not a power of two: " << config->fileAlign;
  }

  // Handle /stack
  if (auto *arg = args.getLastArg(OPT_stack))
    parseNumbers(arg->getValue(), &config->stackReserve, &config->stackCommit);

  // Handle /guard:cf
  if (auto *arg = args.getLastArg(OPT_guard))
    parseGuard(arg->getValue());

  // Handle /heap
  if (auto *arg = args.getLastArg(OPT_heap))
    parseNumbers(arg->getValue(), &config->heapReserve, &config->heapCommit);

  // Handle /version
  if (auto *arg = args.getLastArg(OPT_version))
    parseVersion(arg->getValue(), &config->majorImageVersion,
                 &config->minorImageVersion);

  // Handle /subsystem
  if (auto *arg = args.getLastArg(OPT_subsystem))
    parseSubsystem(arg->getValue(), &config->subsystem,
                   &config->majorSubsystemVersion,
                   &config->minorSubsystemVersion);

  // Handle /osversion
  if (auto *arg = args.getLastArg(OPT_osversion)) {
    parseVersion(arg->getValue(), &config->majorOSVersion,
                 &config->minorOSVersion);
  } else {
    config->majorOSVersion = config->majorSubsystemVersion;
    config->minorOSVersion = config->minorSubsystemVersion;
  }

  // Handle /timestamp
  if (llvm::opt::Arg *arg = args.getLastArg(OPT_timestamp, OPT_repro)) {
    if (arg->getOption().getID() == OPT_repro) {
      config->timestamp = 0;
      config->repro = true;
    } else {
      config->repro = false;
      StringRef value(arg->getValue());
      if (value.getAsInteger(0, config->timestamp))
        Fatal(ctx) << "invalid timestamp: " << value
                   << ".  Expected 32-bit integer";
    }
  } else {
    config->repro = false;
    if (std::optional<std::string> epoch =
            Process::GetEnv("SOURCE_DATE_EPOCH")) {
      StringRef value(*epoch);
      if (value.getAsInteger(0, config->timestamp))
        Fatal(ctx) << "invalid SOURCE_DATE_EPOCH timestamp: " << value
                   << ".  Expected 32-bit integer";
    } else {
      config->timestamp = time(nullptr);
    }
  }

  // Handle /alternatename
  for (auto *arg : args.filtered(OPT_alternatename))
    ctx.symtab.parseAlternateName(arg->getValue());

  // Handle /include
  for (auto *arg : args.filtered(OPT_incl))
    ctx.symtab.addGCRoot(arg->getValue());

  // Handle /implib
  if (auto *arg = args.getLastArg(OPT_implib))
    config->implib = arg->getValue();

  config->noimplib = args.hasArg(OPT_noimplib);

  if (args.hasArg(OPT_profile))
    doGC = true;
  // Handle /opt.
  std::optional<ICFLevel> icfLevel;
  if (args.hasArg(OPT_profile))
    icfLevel = ICFLevel::None;
  unsigned tailMerge = 1;
  bool ltoDebugPM = false;
  for (auto *arg : args.filtered(OPT_opt)) {
    std::string str = StringRef(arg->getValue()).lower();
    SmallVector<StringRef, 1> vec;
    StringRef(str).split(vec, ',');
    for (StringRef s : vec) {
      if (s == "ref") {
        doGC = true;
      } else if (s == "noref") {
        doGC = false;
      } else if (s == "icf" || s.starts_with("icf=")) {
        icfLevel = ICFLevel::All;
      } else if (s == "safeicf") {
        icfLevel = ICFLevel::Safe;
      } else if (s == "noicf") {
        icfLevel = ICFLevel::None;
      } else if (s == "lldtailmerge") {
        tailMerge = 2;
      } else if (s == "nolldtailmerge") {
        tailMerge = 0;
      } else if (s == "ltodebugpassmanager") {
        ltoDebugPM = true;
      } else if (s == "noltodebugpassmanager") {
        ltoDebugPM = false;
      } else if (s.consume_front("lldlto=")) {
        if (s.getAsInteger(10, config->ltoo) || config->ltoo > 3)
          Err(ctx) << "/opt:lldlto: invalid optimization level: " << s;
      } else if (s.consume_front("lldltocgo=")) {
        config->ltoCgo.emplace();
        if (s.getAsInteger(10, *config->ltoCgo) || *config->ltoCgo > 3)
          Err(ctx) << "/opt:lldltocgo: invalid codegen optimization level: "
                   << s;
      } else if (s.consume_front("lldltojobs=")) {
        if (!get_threadpool_strategy(s))
          Err(ctx) << "/opt:lldltojobs: invalid job count: " << s;
        config->thinLTOJobs = s.str();
      } else if (s.consume_front("lldltopartitions=")) {
        if (s.getAsInteger(10, config->ltoPartitions) ||
            config->ltoPartitions == 0)
          Err(ctx) << "/opt:lldltopartitions: invalid partition count: " << s;
      } else if (s != "lbr" && s != "nolbr")
        Err(ctx) << "/opt: unknown option: " << s;
    }
  }

  if (!icfLevel)
    icfLevel = doGC ? ICFLevel::All : ICFLevel::None;
  config->doGC = doGC;
  config->doICF = *icfLevel;
  config->tailMerge =
      (tailMerge == 1 && config->doICF != ICFLevel::None) || tailMerge == 2;
  config->ltoDebugPassManager = ltoDebugPM;

  // Handle /lldsavetemps
  if (args.hasArg(OPT_lldsavetemps)) {
    config->saveTempsArgs.insert_range(lldsaveTempsValues);
  } else {
    for (auto *arg : args.filtered(OPT_lldsavetemps_colon)) {
      StringRef s = arg->getValue();
      if (llvm::is_contained(lldsaveTempsValues, s))
        config->saveTempsArgs.insert(s);
      else
        Err(ctx) << "unknown /lldsavetemps value: " << s;
    }
  }

  // Handle /lldemit
  if (auto *arg = args.getLastArg(OPT_lldemit)) {
    StringRef s = arg->getValue();
    if (s == "obj")
      config->emit = EmitKind::Obj;
    else if (s == "llvm")
      config->emit = EmitKind::LLVM;
    else if (s == "asm")
      config->emit = EmitKind::ASM;
    else
      Err(ctx) << "/lldemit: unknown option: " << s;
  }

  // Handle /kill-at
  if (args.hasArg(OPT_kill_at))
    config->killAt = true;

  // Handle /lldltocache
  if (auto *arg = args.getLastArg(OPT_lldltocache))
    config->ltoCache = arg->getValue();

  // Handle /lldsavecachepolicy
  if (auto *arg = args.getLastArg(OPT_lldltocachepolicy))
    config->ltoCachePolicy = CHECK(
        parseCachePruningPolicy(arg->getValue()),
        Twine("/lldltocachepolicy: invalid cache policy: ") + arg->getValue());

  // Handle /failifmismatch
  for (auto *arg : args.filtered(OPT_failifmismatch))
    checkFailIfMismatch(arg->getValue(), nullptr);

  // Handle /merge
  for (auto *arg : args.filtered(OPT_merge))
    parseMerge(arg->getValue());

  // Add default section merging rules after user rules. User rules take
  // precedence, but we will emit a warning if there is a conflict.
  parseMerge(".idata=.rdata");
  parseMerge(".didat=.rdata");
  parseMerge(".edata=.rdata");
  parseMerge(".xdata=.rdata");
  parseMerge(".00cfg=.rdata");
  parseMerge(".bss=.data");

  if (isArm64EC(config->machine))
    parseMerge(".wowthk=.text");

  if (config->mingw) {
    parseMerge(".ctors=.rdata");
    parseMerge(".dtors=.rdata");
    parseMerge(".CRT=.rdata");
    parseMerge(".data_cygwin_nocopy=.data");
  }

  // Handle /section
  for (auto *arg : args.filtered(OPT_section))
    parseSection(arg->getValue());

  // Handle /align
  if (auto *arg = args.getLastArg(OPT_align)) {
    parseNumbers(arg->getValue(), &config->align);
    if (!isPowerOf2_64(config->align))
      Err(ctx) << "/align: not a power of two: " << StringRef(arg->getValue());
    if (!args.hasArg(OPT_driver))
      Warn(ctx) << "/align specified without /driver; image may not run";
  }

  // Handle /aligncomm
  for (auto *arg : args.filtered(OPT_aligncomm))
    ctx.symtab.parseAligncomm(arg->getValue());

  // Handle /manifestdependency.
  for (auto *arg : args.filtered(OPT_manifestdependency))
    config->manifestDependencies.insert(arg->getValue());

  // Handle /manifest and /manifest:
  if (auto *arg = args.getLastArg(OPT_manifest, OPT_manifest_colon)) {
    if (arg->getOption().getID() == OPT_manifest)
      config->manifest = Configuration::SideBySide;
    else
      parseManifest(arg->getValue());
  }

  // Handle /manifestuac
  if (auto *arg = args.getLastArg(OPT_manifestuac))
    parseManifestUAC(arg->getValue());

  // Handle /manifestfile
  if (auto *arg = args.getLastArg(OPT_manifestfile))
    config->manifestFile = arg->getValue();

  // Handle /manifestinput
  for (auto *arg : args.filtered(OPT_manifestinput))
    config->manifestInput.push_back(arg->getValue());

  if (!config->manifestInput.empty() &&
      config->manifest != Configuration::Embed) {
    Fatal(ctx) << "/manifestinput: requires /manifest:embed";
  }

  // Handle /thinlto-distributor:<path>
  config->dtltoDistributor = args.getLastArgValue(OPT_thinlto_distributor);

  // Handle /thinlto-distributor-arg:<arg>
  for (auto *arg : args.filtered(OPT_thinlto_distributor_arg))
    config->dtltoDistributorArgs.push_back(arg->getValue());

  // Handle /thinlto-remote-compiler:<path>
  config->dtltoCompiler = args.getLastArgValue(OPT_thinlto_compiler);
  if (!config->dtltoDistributor.empty() && config->dtltoCompiler.empty())
    Err(ctx) << "A value must be specified for /thinlto-remote-compiler if "
                "/thinlto-distributor is specified.";

  // Handle /thinlto-remote-compiler-arg:<arg>
  for (auto *arg : args.filtered(OPT_thinlto_compiler_arg))
    config->dtltoCompilerArgs.push_back(arg->getValue());

  // Handle /dwodir
  config->dwoDir = args.getLastArgValue(OPT_dwodir);

  config->thinLTOEmitImportsFiles = args.hasArg(OPT_thinlto_emit_imports_files);
  config->thinLTOIndexOnly = args.hasArg(OPT_thinlto_index_only) ||
                             args.hasArg(OPT_thinlto_index_only_arg);
  config->thinLTOIndexOnlyArg =
      args.getLastArgValue(OPT_thinlto_index_only_arg);
  std::tie(config->thinLTOPrefixReplaceOld, config->thinLTOPrefixReplaceNew,
           config->thinLTOPrefixReplaceNativeObject) =
      getOldNewOptionsExtra(ctx, args, OPT_thinlto_prefix_replace);
  config->thinLTOObjectSuffixReplace =
      getOldNewOptions(ctx, args, OPT_thinlto_object_suffix_replace);
  config->ltoObjPath = args.getLastArgValue(OPT_lto_obj_path);
  config->ltoCSProfileGenerate = args.hasArg(OPT_lto_cs_profile_generate);
  config->ltoCSProfileFile = args.getLastArgValue(OPT_lto_cs_profile_file);
  config->ltoSampleProfileName = args.getLastArgValue(OPT_lto_sample_profile);
  // Handle miscellaneous boolean flags.
  config->ltoPGOWarnMismatch = args.hasFlag(OPT_lto_pgo_warn_mismatch,
                                            OPT_lto_pgo_warn_mismatch_no, true);
  config->allowBind = args.hasFlag(OPT_allowbind, OPT_allowbind_no, true);
  config->allowIsolation =
      args.hasFlag(OPT_allowisolation, OPT_allowisolation_no, true);
  config->incremental =
      args.hasFlag(OPT_incremental, OPT_incremental_no,
                   !config->doGC && config->doICF == ICFLevel::None &&
                       !args.hasArg(OPT_order) && !args.hasArg(OPT_profile));
  config->integrityCheck =
      args.hasFlag(OPT_integritycheck, OPT_integritycheck_no, false);
  config->cetCompat = args.hasFlag(OPT_cetcompat, OPT_cetcompat_no, false);
  config->nxCompat = args.hasFlag(OPT_nxcompat, OPT_nxcompat_no, true);
  for (auto *arg : args.filtered(OPT_swaprun))
    parseSwaprun(arg->getValue());
  config->terminalServerAware =
      !config->dll && args.hasFlag(OPT_tsaware, OPT_tsaware_no, true);
  config->autoImport =
      args.hasFlag(OPT_auto_import, OPT_auto_import_no, config->mingw);
  config->pseudoRelocs = args.hasFlag(
      OPT_runtime_pseudo_reloc, OPT_runtime_pseudo_reloc_no, config->mingw);
  config->callGraphProfileSort = args.hasFlag(
      OPT_call_graph_profile_sort, OPT_call_graph_profile_sort_no, true);
  config->stdcallFixup =
      args.hasFlag(OPT_stdcall_fixup, OPT_stdcall_fixup_no, config->mingw);
  config->warnStdcallFixup = !args.hasArg(OPT_stdcall_fixup);
  config->allowDuplicateWeak =
      args.hasFlag(OPT_lld_allow_duplicate_weak,
                   OPT_lld_allow_duplicate_weak_no, config->mingw);

  if (args.hasFlag(OPT_inferasanlibs, OPT_inferasanlibs_no, false))
    Warn(ctx) << "ignoring '/inferasanlibs', this flag is not supported";

  if (config->incremental && args.hasArg(OPT_profile)) {
    Warn(ctx) << "ignoring '/incremental' due to '/profile' specification";
    config->incremental = false;
  }

  if (config->incremental && args.hasArg(OPT_order)) {
    Warn(ctx) << "ignoring '/incremental' due to '/order' specification";
    config->incremental = false;
  }

  if (config->incremental && config->doGC) {
    Warn(ctx) << "ignoring '/incremental' because REF is enabled; use "
                 "'/opt:noref' to "
                 "disable";
    config->incremental = false;
  }

  if (config->incremental && config->doICF != ICFLevel::None) {
    Warn(ctx) << "ignoring '/incremental' because ICF is enabled; use "
                 "'/opt:noicf' to "
                 "disable";
    config->incremental = false;
  }

  if (errCount(ctx))
    return;

  std::set<sys::fs::UniqueID> wholeArchives;
  for (auto *arg : args.filtered(OPT_wholearchive_file))
    if (std::optional<StringRef> path = findFile(arg->getValue()))
      if (std::optional<sys::fs::UniqueID> id = getUniqueID(*path))
        wholeArchives.insert(*id);

  // A predicate returning true if a given path is an argument for
  // /wholearchive:, or /wholearchive is enabled globally.
  // This function is a bit tricky because "foo.obj /wholearchive:././foo.obj"
  // needs to be handled as "/wholearchive:foo.obj foo.obj".
  auto isWholeArchive = [&](StringRef path) -> bool {
    if (args.hasArg(OPT_wholearchive_flag))
      return true;
    if (std::optional<sys::fs::UniqueID> id = getUniqueID(path))
      return wholeArchives.count(*id);
    return false;
  };

  // Create a list of input files. These can be given as OPT_INPUT options
  // and OPT_wholearchive_file options, and we also need to track OPT_start_lib
  // and OPT_end_lib.
  {
    llvm::TimeTraceScope timeScope2("Parse & queue inputs");
    bool inLib = false;
    for (auto *arg : args) {
      switch (arg->getOption().getID()) {
      case OPT_end_lib:
        if (!inLib)
          Err(ctx) << "stray " << arg->getSpelling();
        inLib = false;
        break;
      case OPT_start_lib:
        if (inLib)
          Err(ctx) << "nested " << arg->getSpelling();
        inLib = true;
        break;
      case OPT_wholearchive_file:
        if (std::optional<StringRef> path = findFileIfNew(arg->getValue()))
          enqueuePath(*path, true, inLib);
        break;
      case OPT_INPUT:
        if (std::optional<StringRef> path = findFileIfNew(arg->getValue()))
          enqueuePath(*path, isWholeArchive(*path), inLib);
        break;
      default:
        // Ignore other options.
        break;
      }
    }
  }

  // Read all input files given via the command line.
  run();
  if (errorCount())
    return;

  // We should have inferred a machine type by now from the input files, but if
  // not we assume x64.
  if (config->machine == IMAGE_FILE_MACHINE_UNKNOWN) {
    Warn(ctx) << "/machine is not specified. x64 is assumed";
    setMachine(AMD64);
  }
  config->wordsize = config->is64() ? 8 : 4;

  if (config->printSearchPaths) {
    SmallString<256> buffer;
    raw_svector_ostream stream(buffer);
    stream << "Library search paths:\n";

    for (StringRef path : searchPaths) {
      if (path == "")
        path = "(cwd)";
      stream << "  " << path << "\n";
    }

    Msg(ctx) << buffer;
  }

  // Process files specified as /defaultlib. These must be processed after
  // addWinSysRootLibSearchPaths(), which is why they are in a separate loop.
  for (auto *arg : args.filtered(OPT_defaultlib))
    if (std::optional<StringRef> path = findLibIfNew(arg->getValue()))
      enqueuePath(*path, false, false);
  run();
  if (errorCount())
    return;

  // Handle /RELEASE
  if (args.hasArg(OPT_release))
    config->writeCheckSum = true;

  // Handle /safeseh, x86 only, on by default, except for mingw.
  if (config->machine == I386) {
    config->safeSEH = args.hasFlag(OPT_safeseh, OPT_safeseh_no, !config->mingw);
    config->noSEH = args.hasArg(OPT_noseh);
  }

  // Handle /stub
  if (auto *arg = args.getLastArg(OPT_stub))
    parseDosStub(arg->getValue());

  // Handle /functionpadmin
  for (auto *arg : args.filtered(OPT_functionpadmin, OPT_functionpadmin_opt))
    parseFunctionPadMin(arg);

  // Handle /dependentloadflag
  for (auto *arg :
       args.filtered(OPT_dependentloadflag, OPT_dependentloadflag_opt))
    parseDependentLoadFlags(arg);

  if (tar) {
    llvm::TimeTraceScope timeScope("Reproducer: response file");
    tar->append(
        "response.txt",
        createResponseFile(args, ArrayRef<StringRef>(searchPaths).slice(1)));
  }

  // Handle /largeaddressaware
  config->largeAddressAware = args.hasFlag(
      OPT_largeaddressaware, OPT_largeaddressaware_no, config->is64());

  // Handle /highentropyva
  config->highEntropyVA =
      config->is64() &&
      args.hasFlag(OPT_highentropyva, OPT_highentropyva_no, true);

  if (!config->dynamicBase &&
      (config->machine == ARMNT || isAnyArm64(config->machine)))
    Err(ctx) << "/dynamicbase:no is not compatible with "
             << machineToStr(config->machine);

  // Handle /export
  {
    llvm::TimeTraceScope timeScope("Parse /export");
    for (auto *arg : args.filtered(OPT_export)) {
      Export e = parseExport(arg->getValue());
      if (config->machine == I386) {
        if (!isDecorated(e.name))
          e.name = saver().save("_" + e.name);
        if (!e.extName.empty() && !isDecorated(e.extName))
          e.extName = saver().save("_" + e.extName);
      }
      ctx.symtab.exports.push_back(e);
    }
  }

  // Handle /def
  if (auto *arg = args.getLastArg(OPT_deffile)) {
    // parseModuleDefs mutates Config object.
    ctx.symtab.parseModuleDefs(arg->getValue());
    if (ctx.config.machine == ARM64X) {
      // MSVC ignores the /defArm64Native argument on non-ARM64X targets.
      // It is also ignored if the /def option is not specified.
      if (auto *arg = args.getLastArg(OPT_defarm64native))
        ctx.hybridSymtab->parseModuleDefs(arg->getValue());
    }
  }

  // Handle generation of import library from a def file.
  if (!args.hasArg(OPT_INPUT, OPT_wholearchive_file)) {
    ctx.forEachSymtab([](SymbolTable &symtab) { symtab.fixupExports(); });
    if (!config->noimplib)
      createImportLibrary(/*asLib=*/true);
    return;
  }

  // Windows specific -- if no /subsystem is given, we need to infer
  // that from entry point name.  Must happen before /entry handling,
  // and after the early return when just writing an import library.
  if (config->subsystem == IMAGE_SUBSYSTEM_UNKNOWN) {
    llvm::TimeTraceScope timeScope("Infer subsystem");
    config->subsystem = ctx.symtab.inferSubsystem();
    if (config->subsystem == IMAGE_SUBSYSTEM_UNKNOWN)
      Fatal(ctx) << "subsystem must be defined";
  }

  // Handle /entry and /dll
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    llvm::TimeTraceScope timeScope("Entry point");
    if (auto *arg = args.getLastArg(OPT_entry)) {
      if (!arg->getValue()[0])
        Fatal(ctx) << "missing entry point symbol name";
      symtab.entry = symtab.addGCRoot(symtab.mangle(arg->getValue()), true);
    } else if (!symtab.entry && !config->noEntry) {
      if (args.hasArg(OPT_dll)) {
        StringRef s = (config->machine == I386) ? "__DllMainCRTStartup@12"
                                                : "_DllMainCRTStartup";
        symtab.entry = symtab.addGCRoot(s, true);
      } else if (config->driverWdm) {
        // /driver:wdm implies /entry:_NtProcessStartup
        symtab.entry =
            symtab.addGCRoot(symtab.mangle("_NtProcessStartup"), true);
      } else {
        // Windows specific -- If entry point name is not given, we need to
        // infer that from user-defined entry name.
        StringRef s = symtab.findDefaultEntry();
        if (s.empty())
          Fatal(ctx) << "entry point must be defined";
        symtab.entry = symtab.addGCRoot(s, true);
        Log(ctx) << "Entry name inferred: " << s;
      }
    }
  });

  // Handle /delayload
  {
    llvm::TimeTraceScope timeScope("Delay load");
    for (auto *arg : args.filtered(OPT_delayload)) {
      config->delayLoads.insert(StringRef(arg->getValue()).lower());
      ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
        if (symtab.machine == I386) {
          symtab.delayLoadHelper = symtab.addGCRoot("___delayLoadHelper2@8");
        } else {
          symtab.delayLoadHelper = symtab.addGCRoot("__delayLoadHelper2", true);
        }
      });
    }
  }

  // Set default image name if neither /out or /def set it.
  if (config->outputFile.empty()) {
    config->outputFile = getOutputPath(
        (*args.filtered(OPT_INPUT, OPT_wholearchive_file).begin())->getValue(),
        config->dll, config->driver);
  }

  // Fail early if an output file is not writable.
  if (auto e = tryCreateFile(config->outputFile)) {
    Err(ctx) << "cannot open output file " << config->outputFile << ": "
             << e.message();
    return;
  }

  config->lldmapFile = getMapFile(args, OPT_lldmap, OPT_lldmap_file);
  config->mapFile = getMapFile(args, OPT_map, OPT_map_file);

  if (config->mapFile != "" && args.hasArg(OPT_map_info)) {
    for (auto *arg : args.filtered(OPT_map_info)) {
      std::string s = StringRef(arg->getValue()).lower();
      if (s == "exports")
        config->mapInfo = true;
      else
        Err(ctx) << "unknown option: /mapinfo:" << s;
    }
  }

  if (config->lldmapFile != "" && config->lldmapFile == config->mapFile) {
    Warn(ctx) << "/lldmap and /map have the same output file '"
              << config->mapFile << "'.\n>>> ignoring /lldmap";
    config->lldmapFile.clear();
  }

  // If should create PDB, use the hash of PDB content for build id. Otherwise,
  // generate using the hash of executable content.
  if (args.hasFlag(OPT_build_id, OPT_build_id_no, false))
    config->buildIDHash = BuildIDHash::Binary;

  if (shouldCreatePDB) {
    // Put the PDB next to the image if no /pdb flag was passed.
    if (config->pdbPath.empty()) {
      config->pdbPath = config->outputFile;
      sys::path::replace_extension(config->pdbPath, ".pdb");
    }

    // The embedded PDB path should be the absolute path to the PDB if no
    // /pdbaltpath flag was passed.
    if (config->pdbAltPath.empty()) {
      config->pdbAltPath = config->pdbPath;

      // It's important to make the path absolute and remove dots.  This path
      // will eventually be written into the PE header, and certain Microsoft
      // tools won't work correctly if these assumptions are not held.
      sys::fs::make_absolute(config->pdbAltPath);
      sys::path::remove_dots(config->pdbAltPath);
    } else {
      // Don't do this earlier, so that ctx.OutputFile is ready.
      parsePDBAltPath();
    }
    config->buildIDHash = BuildIDHash::PDB;
  }

  // Set default image base if /base is not given.
  if (config->imageBase == uint64_t(-1))
    config->imageBase = getDefaultImageBase();

  ctx.forEachSymtab([&](SymbolTable &symtab) {
    symtab.addSynthetic(symtab.mangle("__ImageBase"), nullptr);
    if (symtab.machine == I386) {
      symtab.addAbsolute("___safe_se_handler_table", 0);
      symtab.addAbsolute("___safe_se_handler_count", 0);
    }

    symtab.addAbsolute(symtab.mangle("__guard_fids_count"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_fids_table"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_flags"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_iat_count"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_iat_table"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_longjmp_count"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_longjmp_table"), 0);
    // Needed for MSVC 2017 15.5 CRT.
    symtab.addAbsolute(symtab.mangle("__enclave_config"), 0);
    // Needed for MSVC 2019 16.8 CRT.
    symtab.addAbsolute(symtab.mangle("__guard_eh_cont_count"), 0);
    symtab.addAbsolute(symtab.mangle("__guard_eh_cont_table"), 0);

    if (symtab.isEC()) {
      symtab.addAbsolute("__arm64x_extra_rfe_table", 0);
      symtab.addAbsolute("__arm64x_extra_rfe_table_size", 0);
      symtab.addAbsolute("__arm64x_redirection_metadata", 0);
      symtab.addAbsolute("__arm64x_redirection_metadata_count", 0);
      symtab.addAbsolute("__hybrid_auxiliary_delayload_iat_copy", 0);
      symtab.addAbsolute("__hybrid_auxiliary_delayload_iat", 0);
      symtab.addAbsolute("__hybrid_auxiliary_iat", 0);
      symtab.addAbsolute("__hybrid_auxiliary_iat_copy", 0);
      symtab.addAbsolute("__hybrid_code_map", 0);
      symtab.addAbsolute("__hybrid_code_map_count", 0);
      symtab.addAbsolute("__hybrid_image_info_bitfield", 0);
      symtab.addAbsolute("__x64_code_ranges_to_entry_points", 0);
      symtab.addAbsolute("__x64_code_ranges_to_entry_points_count", 0);
      symtab.addSynthetic("__guard_check_icall_a64n_fptr", nullptr);
      symtab.addSynthetic("__arm64x_native_entrypoint", nullptr);
    }

    if (config->pseudoRelocs) {
      symtab.addAbsolute(symtab.mangle("__RUNTIME_PSEUDO_RELOC_LIST__"), 0);
      symtab.addAbsolute(symtab.mangle("__RUNTIME_PSEUDO_RELOC_LIST_END__"), 0);
    }
    if (config->mingw) {
      symtab.addAbsolute(symtab.mangle("__CTOR_LIST__"), 0);
      symtab.addAbsolute(symtab.mangle("__DTOR_LIST__"), 0);
      symtab.addAbsolute("__data_start__", 0);
      symtab.addAbsolute("__data_end__", 0);
      symtab.addAbsolute("__bss_start__", 0);
      symtab.addAbsolute("__bss_end__", 0);
    }
    if (config->debug || config->buildIDHash != BuildIDHash::None)
      if (symtab.findUnderscore("__buildid"))
        symtab.addUndefined(symtab.mangle("__buildid"));
  });

  // This code may add new undefined symbols to the link, which may enqueue more
  // symbol resolution tasks, so we need to continue executing tasks until we
  // converge.
  {
    llvm::TimeTraceScope timeScope("Add unresolved symbols");
    do {
      ctx.forEachSymtab([&](SymbolTable &symtab) {
        // Windows specific -- if entry point is not found,
        // search for its mangled names.
        if (symtab.entry)
          symtab.mangleMaybe(symtab.entry);

        // Windows specific -- Make sure we resolve all dllexported symbols.
        for (Export &e : symtab.exports) {
          if (!e.forwardTo.empty())
            continue;
          e.sym = symtab.addGCRoot(e.name, !e.data);
          if (e.source != ExportSource::Directives)
            e.symbolName = symtab.mangleMaybe(e.sym);
        }

        symtab.resolveAlternateNames();
      });

      ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
        // If any inputs are bitcode files, the LTO code generator may create
        // references to library functions that are not explicit in the bitcode
        // file's symbol table. If any of those library functions are defined in
        // a bitcode file in an archive member, we need to arrange to use LTO to
        // compile those archive members by adding them to the link beforehand.
        if (!symtab.bitcodeFileInstances.empty()) {
          llvm::Triple TT(
              symtab.bitcodeFileInstances.front()->obj->getTargetTriple());
          for (auto *s : lto::LTO::getRuntimeLibcallSymbols(TT))
            symtab.addLibcall(s);
        }

        // Windows specific -- if __load_config_used can be resolved, resolve
        // it.
        if (symtab.findUnderscore("_load_config_used"))
          symtab.addGCRoot(symtab.mangle("_load_config_used"));

        if (args.hasArg(OPT_include_optional)) {
          // Handle /includeoptional
          for (auto *arg : args.filtered(OPT_include_optional))
            if (isa_and_nonnull<LazyArchive>(symtab.find(arg->getValue())))
              symtab.addGCRoot(arg->getValue());
        }
      });
    } while (run());
  }

  // Handle /includeglob
  for (StringRef pat : args::getStrings(args, OPT_incl_glob))
    ctx.forEachActiveSymtab(
        [&](SymbolTable &symtab) { symtab.addUndefinedGlob(pat); });

  // Create wrapped symbols for -wrap option.
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    addWrappedSymbols(symtab, args);
    // Load more object files that might be needed for wrapped symbols.
    if (!symtab.wrapped.empty())
      while (run())
        ;
  });

  if (config->autoImport || config->stdcallFixup) {
    // MinGW specific.
    // Load any further object files that might be needed for doing automatic
    // imports, and do stdcall fixups.
    //
    // For cases with no automatically imported symbols, this iterates once
    // over the symbol table and doesn't do anything.
    //
    // For the normal case with a few automatically imported symbols, this
    // should only need to be run once, since each new object file imported
    // is an import library and wouldn't add any new undefined references,
    // but there's nothing stopping the __imp_ symbols from coming from a
    // normal object file as well (although that won't be used for the
    // actual autoimport later on). If this pass adds new undefined references,
    // we won't iterate further to resolve them.
    //
    // If stdcall fixups only are needed for loading import entries from
    // a DLL without import library, this also just needs running once.
    // If it ends up pulling in more object files from static libraries,
    // (and maybe doing more stdcall fixups along the way), this would need
    // to loop these two calls.
    ctx.forEachSymtab([](SymbolTable &symtab) { symtab.loadMinGWSymbols(); });
    run();
  }

  // At this point, we should not have any symbols that cannot be resolved.
  // If we are going to do codegen for link-time optimization, check for
  // unresolvable symbols first, so we don't spend time generating code that
  // will fail to link anyway.
  if (!config->forceUnresolved)
    ctx.forEachSymtab([](SymbolTable &symtab) {
      if (!symtab.bitcodeFileInstances.empty())
        symtab.reportUnresolvable();
    });
  if (errorCount())
    return;

  ctx.forEachSymtab([](SymbolTable &symtab) {
    symtab.hadExplicitExports = !symtab.exports.empty();
  });
  if (config->mingw) {
    // In MinGW, all symbols are automatically exported if no symbols
    // are chosen to be exported.
    maybeExportMinGWSymbols(args);
  }

  // Do LTO by compiling bitcode input files to a set of native COFF files then
  // link those files (unless -thinlto-index-only was given, in which case we
  // resolve symbols and write indices, but don't generate native code or link).
  ltoCompilationDone = true;
  ctx.forEachSymtab([](SymbolTable &symtab) { symtab.compileBitcodeFiles(); });

  if (Defined *d =
          dyn_cast_or_null<Defined>(ctx.symtab.findUnderscore("_tls_used")))
    config->gcroot.push_back(d);

  // If -thinlto-index-only is given, we should create only "index
  // files" and not object files. Index file creation is already done
  // in addCombinedLTOObject, so we are done if that's the case.
  // Likewise, don't emit object files for other /lldemit options.
  if (config->emit != EmitKind::Obj || config->thinLTOIndexOnly)
    return;

  // If we generated native object files from bitcode files, this resolves
  // references to the symbols we use from them.
  run();

  // Apply symbol renames for -wrap.
  ctx.forEachSymtab([](SymbolTable &symtab) {
    if (!symtab.wrapped.empty())
      wrapSymbols(symtab);
  });

  if (isArm64EC(config->machine))
    createECExportThunks();

  // Resolve remaining undefined symbols and warn about imported locals.
  ctx.forEachSymtab(
      [&](SymbolTable &symtab) { symtab.resolveRemainingUndefines(); });

  if (errorCount())
    return;

  if (config->mingw) {
    // Make sure the crtend.o object is the last object file. This object
    // file can contain terminating section chunks that need to be placed
    // last. GNU ld processes files and static libraries explicitly in the
    // order provided on the command line, while lld will pull in needed
    // files from static libraries only after the last object file on the
    // command line.
    for (auto i = ctx.objFileInstances.begin(), e = ctx.objFileInstances.end();
         i != e; i++) {
      ObjFile *file = *i;
      if (isCrtend(file->getName())) {
        ctx.objFileInstances.erase(i);
        ctx.objFileInstances.push_back(file);
        break;
      }
    }
  }

  // Windows specific -- when we are creating a .dll file, we also
  // need to create a .lib file. In MinGW mode, we only do that when the
  // -implib option is given explicitly, for compatibility with GNU ld.
  if (config->dll || !ctx.symtab.exports.empty() ||
      (ctx.config.machine == ARM64X && !ctx.hybridSymtab->exports.empty())) {
    llvm::TimeTraceScope timeScope("Create .lib exports");
    ctx.forEachActiveSymtab([](SymbolTable &symtab) { symtab.fixupExports(); });
    if (!config->noimplib && (!config->mingw || !config->implib.empty()))
      createImportLibrary(/*asLib=*/false);
    ctx.forEachActiveSymtab(
        [](SymbolTable &symtab) { symtab.assignExportOrdinals(); });
  }

  // Handle /output-def (MinGW specific).
  if (auto *arg = args.getLastArg(OPT_output_def))
    writeDefFile(ctx, arg->getValue(), ctx.symtab.exports);

  // Set extra alignment for .comm symbols
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    for (auto pair : symtab.alignComm) {
      StringRef name = pair.first;
      uint32_t alignment = pair.second;

      Symbol *sym = symtab.find(name);
      if (!sym) {
        Warn(ctx) << "/aligncomm symbol " << name << " not found";
        continue;
      }

      // If the symbol isn't common, it must have been replaced with a regular
      // symbol, which will carry its own alignment.
      auto *dc = dyn_cast<DefinedCommon>(sym);
      if (!dc)
        continue;

      CommonChunk *c = dc->getChunk();
      c->setAlignment(std::max(c->getAlignment(), alignment));
    }
  });

  // Windows specific -- Create an embedded or side-by-side manifest.
  // /manifestdependency: enables /manifest unless an explicit /manifest:no is
  // also passed.
  if (config->manifest == Configuration::Embed)
    addBuffer(createManifestRes(), false, false);
  else if (config->manifest == Configuration::SideBySide ||
           (config->manifest == Configuration::Default &&
            !config->manifestDependencies.empty()))
    createSideBySideManifest();

  // Handle /order. We want to do this at this moment because we
  // need a complete list of comdat sections to warn on nonexistent
  // functions.
  if (auto *arg = args.getLastArg(OPT_order)) {
    if (args.hasArg(OPT_call_graph_ordering_file))
      Err(ctx) << "/order and /call-graph-order-file may not be used together";
    parseOrderFile(arg->getValue());
    config->callGraphProfileSort = false;
  }

  // Handle /call-graph-ordering-file and /call-graph-profile-sort (default on).
  if (config->callGraphProfileSort) {
    llvm::TimeTraceScope timeScope("Call graph");
    if (auto *arg = args.getLastArg(OPT_call_graph_ordering_file))
      parseCallGraphFile(arg->getValue());
    else
      readCallGraphsFromObjectFiles(ctx);
  }

  // Handle /print-symbol-order.
  if (auto *arg = args.getLastArg(OPT_print_symbol_order))
    config->printSymbolOrder = arg->getValue();

  if (ctx.symtab.isEC())
    ctx.symtab.initializeECThunks();
  ctx.forEachActiveSymtab(
      [](SymbolTable &symtab) { symtab.initializeLoadConfig(); });

  // Identify unreferenced COMDAT sections.
  if (config->doGC) {
    if (config->mingw) {
      // markLive doesn't traverse .eh_frame, but the personality function is
      // only reached that way. The proper solution would be to parse and
      // traverse the .eh_frame section, like the ELF linker does.
      // For now, just manually try to retain the known possible personality
      // functions. This doesn't bring in more object files, but only marks
      // functions that already have been included to be retained.
      ctx.forEachSymtab([&](SymbolTable &symtab) {
        for (const char *n : {"__gxx_personality_v0", "__gcc_personality_v0",
                              "rust_eh_personality"}) {
          Defined *d = dyn_cast_or_null<Defined>(symtab.findUnderscore(n));
          if (d && !d->isGCRoot) {
            d->isGCRoot = true;
            config->gcroot.push_back(d);
          }
        }
      });
    }

    markLive(ctx);
  }

  // Needs to happen after the last call to addFile().
  convertResources();

  // Identify identical COMDAT sections to merge them.
  if (config->doICF != ICFLevel::None) {
    findKeepUniqueSections(ctx);
    doICF(ctx);
  }

  // Write the result.
  writeResult(ctx);

  // Stop early so we can print the results.
  rootTimer.stop();
  if (config->showTiming)
    ctx.rootTimer.print();

  if (config->timeTraceEnabled) {
    // Manually stop the topmost "COFF link" scope, since we're shutting down.
    timeTraceProfilerEnd();

    checkError(timeTraceProfilerWrite(
        args.getLastArgValue(OPT_time_trace_eq).str(), config->outputFile));
    timeTraceProfilerCleanup();
  }
}

} // namespace lld::coff
