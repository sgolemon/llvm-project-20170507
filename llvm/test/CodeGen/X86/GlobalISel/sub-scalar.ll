; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=x86_64-linux-gnu -global-isel -verify-machineinstrs < %s -o - | FileCheck %s --check-prefix=X64

define i64 @test_sub_i64(i64 %arg1, i64 %arg2) {
; X64-LABEL: test_sub_i64:
; X64:       # BB#0:
; X64-NEXT:    subq %rsi, %rdi
; X64-NEXT:    movq %rdi, %rax
; X64-NEXT:    retq
  %ret = sub i64 %arg1, %arg2
  ret i64 %ret
}

define i32 @test_sub_i32(i32 %arg1, i32 %arg2) {
; X64-LABEL: test_sub_i32:
; X64:       # BB#0:
; X64-NEXT:    subl %esi, %edi
; X64-NEXT:    movl %edi, %eax
; X64-NEXT:    retq
  %ret = sub i32 %arg1, %arg2
  ret i32 %ret
}

define i16 @test_sub_i16(i16 %arg1, i16 %arg2) {
; X64-LABEL: test_sub_i16:
; X64:       # BB#0:
; X64-NEXT:    subw %si, %di
; X64-NEXT:    movl %edi, %eax
; X64-NEXT:    retq
  %ret = sub i16 %arg1, %arg2
  ret i16 %ret
}

define i8 @test_sub_i8(i8 %arg1, i8 %arg2) {
; X64-LABEL: test_sub_i8:
; X64:       # BB#0:
; X64-NEXT:    subb %sil, %dil
; X64-NEXT:    movl %edi, %eax
; X64-NEXT:    retq
  %ret = sub i8 %arg1, %arg2
  ret i8 %ret
}

define i32 @test_sub_i1(i32 %arg1, i32 %arg2) {
; X64-LABEL: test_sub_i1:
; X64:       # BB#0:
; X64-NEXT:    subb %sil, %dil
; X64-NEXT:    andl $1, %edi
; X64-NEXT:    movl %edi, %eax
; X64-NEXT:    retq
  %a1 = trunc i32 %arg1 to i1
  %a2 = trunc i32 %arg2 to i1
  %x = sub i1 %a1 , %a2
  %ret = zext i1 %x to i32
  ret i32 %ret
}
