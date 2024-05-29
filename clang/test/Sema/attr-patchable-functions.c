// RUN: %clang_cc1 -triple riscv32-unknown-elf -verify -fsyntax-only -DWARN -o - %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -verify -fsyntax-only -DNOWARN -Wno-ignored-attributes -o - %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -verify -fsyntax-only -DNOWARN -fpatch-indirect -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -verify -fsyntax-only -DWARN -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -verify -fsyntax-only -DNOWARN -Wno-ignored-attributes -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -verify -fsyntax-only -DNOWARN -fpatch-indirect -o - %s
// RUN: not %clang_cc1 -triple riscv32-unknown-elf -fpatch-indirect -fsyntax-only -DERR1 -S %s
// RUN: not %clang_cc1 -triple riscv32-unknown-elf -fpatch-indirect -fsyntax-only -DERR2 -S %s
// RUN: not %clang_cc1 -triple riscv64-unknown-elf -fpatch-indirect -fsyntax-only -DERR1 -S %s
// RUN: not %clang_cc1 -triple riscv64-unknown-elf -fpatch-indirect -fsyntax-only -DERR2 -S %s

#if defined(WARN)

extern int var;
// expected-warning@+1 {{'patchable' attribute ignored; use -fpatch-indirect to enable the attribute}}
int patchFunc() __attribute__((patchable)) {
  return var * var;
}

#elif defined(NOWARN)

// expected-no-diagnostics
extern int var;
int patchFunc() __attribute__((patchable)) {
  return var * var;
}

#endif

#if defined(ERR1)

extern int var;
// expected-error@+1 {{'patchable' attribute cannot be applied to non external linkage function 'patchLocalFunc'}}
static int patchLocalFunc() __attribute__((patchable)) {
  return var * var;
}
int (*fptr) () = patchLocalFunc;

#elif defined(ERR2)

extern int var;
// expected-error@+1 {{'patchable' attribute cannot be applied to non default visible function 'patchHiddenFunc'}}
__attribute__((visibility("hidden"))) int patchHiddenFunc() __attribute__((patchable)) {
  return var * var;
}

#endif
