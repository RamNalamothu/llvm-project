/// -fpatch-indirect is not the default and dso_local is set for global symbols.
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-llvm -Wno-ignored-attributes -mrelocation-model static -o - %s | FileCheck --check-prefixes=NOPATCHABLE %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-llvm -Wno-ignored-attributes -mrelocation-model static -o - %s | FileCheck --check-prefixes=NOPATCHABLE %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-obj -Wno-ignored-attributes -mrelocation-model static -o - %s | llvm-readelf -s - | FileCheck --check-prefixes=NOPATCHABLE_OBJ %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-obj -Wno-ignored-attributes -mrelocation-model static -o - %s | llvm-readelf -s - | FileCheck --check-prefixes=NOPATCHABLE_OBJ %s

/// Enabling -fpatch-indirect sets 'patchable' attribute metadata and will not
/// set 'dso_local' for functions with 'patchable' attribute.
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-llvm -fpatch-indirect -mrelocation-model static -o - %s | FileCheck --check-prefixes=PATCHABLE %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-llvm -fpatch-indirect -mrelocation-model static -o - %s | FileCheck --check-prefixes=PATCHABLE %s
// RUN: %clang_cc1 -triple riscv32-unknown-elf -emit-obj -fpatch-indirect -mrelocation-model static -o - %s | llvm-readelf -s - | FileCheck --check-prefixes=PATCHABLE_OBJ %s
// RUN: %clang_cc1 -triple riscv64-unknown-elf -emit-obj -fpatch-indirect -mrelocation-model static -o - %s | llvm-readelf -s - | FileCheck --check-prefixes=PATCHABLE_OBJ %s

void foo(void) __attribute__((patchable));
void bar(void) __attribute__((patchable));
void baz(void);

// NOPATCHABLE-NOT: Function Attrs{{.*}} patchable
// PATCHABLE: Function Attrs{{.*}} patchable
// NOPATCHABLE: {{.*}} dso_local {{.*}} @foo() #0
// PATCHABLE-NOT: {{.*}} dso_local {{.*}} @foo() #0
void foo(void) __attribute__((patchable)) {}

// NOPATCHABLE-NOT: Function Attrs{{.*}} patchable
// PATCHABLE: Function Attrs{{.*}} patchable
// NOPATCHABLE: {{.*}} dso_local {{.*}} @bar() #0
// PATCHABLE-NOT: {{.*}} dso_local {{.*}} @bar() #0
void bar(void) {}

// NOPATCHABLE-NOT: Function Attrs{{.*}} patchable
// PATCHABLE: Function Attrs{{.*}} patchable
// NOPATCHABLE: {{.*}} dso_local {{.*}} @baz() #0
// PATCHABLE-NOT: {{.*}} dso_local {{.*}} @baz() #0
void baz(void) __attribute__((patchable)) {}

// NOPATCHABLE-NOT: Function Attrs{{.*}} patchable
// PATCHABLE: Function Attrs{{.*}} patchable
// NOPATCHABLE: {{.*}} dso_local {{.*}} @bah() #0
// PATCHABLE-NOT: {{.*}} dso_local {{.*}} @bah() #0
void bah(void) __attribute__((patchable)) {}


// NOPATCHABLE-NOT: attributes #0 = { {{.*}} patchable
// PATCHABLE: attributes #0 = { {{.*}} patchable

// NOPATCHABLE_OBJ-NOT: __llvm_patchable_
// PATCHABLE_OBJ: __llvm_patchable_foo
// PATCHABLE_OBJ: __llvm_patchable_bar
// PATCHABLE_OBJ: __llvm_patchable_baz
// PATCHABLE_OBJ: __llvm_patchable_bah
