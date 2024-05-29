/// Check that -fpatch-indirect is not the default.
// RUN: %clang -target riscv32-unknown-elf %s -### 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -target riscv64-unknown-elf %s -### 2>&1 | FileCheck %s --check-prefix=DEFAULT
// DEFAULT-NOT: "-fpatch-indirect"

/// Check that -fpatch-indirect is honored.
// RUN: %clang -target riscv32-unknown-elf -fpatch-indirect %s -### 2>&1 | FileCheck %s --check-prefix=CHECK
// RUN: %clang -target riscv64-unknown-elf -fpatch-indirect %s -### 2>&1 | FileCheck %s --check-prefix=CHECK
// CHECK: "-fpatch-indirect"

// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -fsemantic-interposition %s 2>&1 | FileCheck %s --check-prefix=ERROR
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -fsemantic-interposition %s 2>&1 | FileCheck %s --check-prefix=ERROR
// ERROR: error: cannot specify '-fsemantic-interposition' along with '-fpatch-indirect'

// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect --patch-base=does_not_matter %s 2>&1 | FileCheck %s --check-prefix=ERROR2
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect --patch-base=does_not_matter %s 2>&1 | FileCheck %s --check-prefix=ERROR2
// ERROR2: error: cannot specify '--patch-base=does_not_matter' along with '-fpatch-indirect'

// RUN: not %clang -target riscv32-unknown-elf --patch-base= -nostdlib -fuse-ld=qcld %s 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not %clang -target riscv64-unknown-elf --patch-base= -nostdlib -fuse-ld=qcld %s 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not %clang -target riscv32-unknown-elf --patch-base=does_not_exist -nostdlib -fuse-ld=qcld %s 2>&1 | FileCheck %s --check-prefix=NOFILE
// RUN: not %clang -target riscv64-unknown-elf --patch-base=does_not_exist -nostdlib -fuse-ld=qcld %s 2>&1 | FileCheck %s --check-prefix=NOFILE
// EMPTY: error: joined argument expects additional value: '--patch-base='
// NOFILE: error: no such file or directory: 'does_not_exist'

// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -fPIC %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -fpic %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -fpie %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -fPIC %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -fpic %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -fpie %s 2>&1 | FileCheck %s --check-prefix=INVALID
// INVALID: error: invalid argument '-fpatch-indirect' only allowed with 'static non-PIE ELF images'

// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -flto=full %s 2>&1 | FileCheck %s --check-prefix=LTOFULL
// RUN: not %clang -target riscv32-unknown-elf -fpatch-indirect -flto=thin %s 2>&1 | FileCheck %s --check-prefix=LTOTHIN
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -flto=full %s 2>&1 | FileCheck %s --check-prefix=LTOFULL
// RUN: not %clang -target riscv64-unknown-elf -fpatch-indirect -flto=thin %s 2>&1 | FileCheck %s --check-prefix=LTOTHIN
// LTOFULL: error: cannot specify '-flto=full' along with '-fpatch-indirect'
// LTOTHIN: error: cannot specify '-flto=thin' along with '-fpatch-indirect'
