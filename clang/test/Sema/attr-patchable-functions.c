// RUN: %clang_cc1 -triple riscv32-unknown-elf -fsyntax-only -verify=generate -o - %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -fsyntax-only -verify=expected -Wno-ignored-attributes -o - %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -fsyntax-only -verify=emit -fpatch-indirect -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -fsyntax-only -verify=generate -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -fsyntax-only -verify=expected -Wno-ignored-attributes -o - %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -fsyntax-only -verify=emit -fpatch-indirect -o - %s

// expected-no-diagnostics

extern int var;

// generate-warning@+1 {{'patchable' attribute ignored; use -fpatch-indirect to enable the attribute}}
int patchFunc() __attribute__((patchable)) {
  return var * var;
}

// generate-warning@+2 {{'patchable' attribute ignored; use -fpatch-indirect to enable the attribute}}
// emit-error@+1 {{'patchable' attribute cannot be applied to non external linkage function 'patchLocalFunc'}}
static int patchLocalFunc() __attribute__((patchable)) {
  return var * var;
}
int (*fptr) () = patchLocalFunc;

// generate-warning@+2 {{'patchable' attribute ignored; use -fpatch-indirect to enable the attribute}}
// emit-error@+1 {{'patchable' attribute cannot be applied to non default visible function 'patchHiddenFunc'}}
__attribute__((visibility("hidden"))) int patchHiddenFunc() __attribute__((patchable)) {
  return var * var;
}

