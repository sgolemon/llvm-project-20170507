; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-linux | FileCheck %s --check-prefix=LNX1
; RUN: llc < %s -mtriple=x86_64-linux-gnux32 | FileCheck %s --check-prefix=LNX2
; RUN: llc < %s -mtriple=x86_64-nacl | FileCheck %s --check-prefix=NACL
; RUN: llc < %s -mtriple=x86_64-win32 | FileCheck %s --check-prefix=WIN

define i64 @test2(i64 %a) {
; LNX1-LABEL: test2:
; LNX1:       # BB#0:
; LNX1-NEXT:    leaq (,%rdi,4), %rax
; LNX1-NEXT:    orq %rdi, %rax
; LNX1-NEXT:    retq
;
; LNX2-LABEL: test2:
; LNX2:       # BB#0:
; LNX2-NEXT:    leaq (,%rdi,4), %rax
; LNX2-NEXT:    orq %rdi, %rax
; LNX2-NEXT:    retq
;
; NACL-LABEL: test2:
; NACL:       # BB#0:
; NACL-NEXT:    leaq (,%rdi,4), %rax
; NACL-NEXT:    orq %rdi, %rax
; NACL-NEXT:    retq
;
; WIN-LABEL: test2:
; WIN:       # BB#0:
; WIN-NEXT:    leaq (,%rcx,4), %rax
; WIN-NEXT:    orq %rcx, %rax
; WIN-NEXT:    retq
  %tmp2 = shl i64 %a, 2
	%tmp3 = or i64 %tmp2, %a
  ret i64 %tmp3
}

define i32 @test(i32 %a) {
; LNX1-LABEL: test:
; LNX1:       # BB#0:
; LNX1-NEXT:    # kill: %EDI<def> %EDI<kill> %RDI<def>
; LNX1-NEXT:    leal (%rdi,%rdi,2), %eax
; LNX1-NEXT:    retq
;
; LNX2-LABEL: test:
; LNX2:       # BB#0:
; LNX2-NEXT:    # kill: %EDI<def> %EDI<kill> %RDI<def>
; LNX2-NEXT:    leal (%rdi,%rdi,2), %eax
; LNX2-NEXT:    retq
;
; NACL-LABEL: test:
; NACL:       # BB#0:
; NACL-NEXT:    # kill: %EDI<def> %EDI<kill> %RDI<def>
; NACL-NEXT:    leal (%rdi,%rdi,2), %eax
; NACL-NEXT:    retq
;
; WIN-LABEL: test:
; WIN:       # BB#0:
; WIN-NEXT:    # kill: %ECX<def> %ECX<kill> %RCX<def>
; WIN-NEXT:    leal (%rcx,%rcx,2), %eax
; WIN-NEXT:    retq
  %tmp2 = mul i32 %a, 3
  ret i32 %tmp2
}

define i64 @test3(i64 %a) {
; LNX1-LABEL: test3:
; LNX1:       # BB#0:
; LNX1-NEXT:    leaq (,%rdi,8), %rax
; LNX1-NEXT:    retq
;
; LNX2-LABEL: test3:
; LNX2:       # BB#0:
; LNX2-NEXT:    leaq (,%rdi,8), %rax
; LNX2-NEXT:    retq
;
; NACL-LABEL: test3:
; NACL:       # BB#0:
; NACL-NEXT:    leaq (,%rdi,8), %rax
; NACL-NEXT:    retq
;
; WIN-LABEL: test3:
; WIN:       # BB#0:
; WIN-NEXT:    leaq (,%rcx,8), %rax
; WIN-NEXT:    retq
  %tmp2 = shl i64 %a, 3
  ret i64 %tmp2
}

