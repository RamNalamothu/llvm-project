// REQUIRES: riscv-registered-target

/// -fpatch-indirect is not the default and dso_local is set for global symbols.
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-llvm -Wno-ignored-attributes -mrelocation-model static -o - %s | FileCheck --implicit-check-not="Function Attrs{{.*}} patchable" --check-prefixes=CHECK,DSOLOCAL %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-llvm -Wno-ignored-attributes -mrelocation-model static -o - %s | FileCheck --implicit-check-not="Function Attrs{{.*}} patchable" --check-prefixes=CHECK,DSOLOCAL %s

/// Enabling -fpatch-indirect sets 'patchable' attribute metadata and will not
/// set 'dso_local' for functions with 'patchable' attribute.
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-llvm -fpatch-indirect -mrelocation-model static -o - %s | FileCheck --implicit-check-not="{{.*}} dso_local {{.*}} #0" --check-prefixes=CHECK,PATCHABLE %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-llvm -fpatch-indirect -mrelocation-model static -o - %s | FileCheck --implicit-check-not="{{.*}} dso_local {{.*}} #0" --check-prefixes=CHECK,PATCHABLE %s

void foo(void) __attribute__((patchable));
void bar(void) __attribute__((patchable));
void baz(void);

// CHECK: target triple =
// PATCHABLE: Function Attrs{{.*}} patchable
// DSOLOCAL: {{.*}} dso_local {{.*}} @foo() #0
void foo(void) __attribute__((patchable)) {}

// PATCHABLE: Function Attrs{{.*}} patchable
// DSOLOCAL: {{.*}} dso_local {{.*}} @bar() #0
void bar(void) {}

// PATCHABLE: Function Attrs{{.*}} patchable
// DSOLOCAL: {{.*}} dso_local {{.*}} @baz() #0
void baz(void) __attribute__((patchable)) {}

// PATCHABLE: Function Attrs{{.*}} patchable
// DSOLOCAL: {{.*}} dso_local {{.*}} @bah() #0
void bah(void) __attribute__((patchable)) {}


// PATCHABLE: attributes #0 = { {{.*}} patchable
// CHECK: llvm.module.flags

