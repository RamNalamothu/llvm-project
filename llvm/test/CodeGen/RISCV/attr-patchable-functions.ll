; RUN: llc -mtriple=riscv32 -verify-machineinstrs < %s | FileCheck --implicit-check-not=__llvm_patchable_bar --check-prefix=ASM %s
; RUN: llc -mtriple=riscv64 -verify-machineinstrs < %s | FileCheck --implicit-check-not=__llvm_patchable_bar --check-prefix=ASM %s
; RUN: llc -mtriple=riscv32 -filetype=obj %s -o - | llvm-readelf -s - | FileCheck --implicit-check-not=__llvm_patchable_bar --check-prefix=OBJ %s
; RUN: llc -mtriple=riscv64 -filetype=obj %s -o - | llvm-readelf -s - | FileCheck --implicit-check-not=__llvm_patchable_bar --check-prefix=OBJ %s

; ASM-LABEL: foo:
; ASM: .globl  __llvm_patchable_foo

; OBJ: Symbol table
; OBJ: __llvm_patchable_foo

define void @foo() #0 {
entry:
  ret void
}

define void @bar() #1 {
entry:
  ret void
}

attributes #0 = { nounwind patchable }
attributes #1 = { nounwind }
