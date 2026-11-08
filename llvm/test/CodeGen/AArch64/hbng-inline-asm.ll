; Inline asm: nothing for an empty statement (compiler barrier), SVC for a
; system call, the system register accesses and barriers as their opcodes
; (mrs alone gives a clean output, msr and barriers nothing), and an
; Unsupported line naming every other instruction instead of dropping it.
; Comments, labels and directives are not instructions.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; CHECK-LABEL: HBNG_barrier_annot:
; CHECK-NEXT:  .ascii "END\n"
define void @barrier() {
  call void asm sideeffect "", "~{memory}"()
  ret void
}

; CHECK-LABEL: HBNG_syscall_annot:
; CHECK:       .ascii "SVC\n"
; CHECK-NEXT:  .ascii "END\n"
define i64 @syscall(i64 %n) {
  %r = call i64 asm sideeffect "svc 0", "={x0},{x8},~{memory}"(i64 %n)
  ret i64 %r
}

; A label, a tab after the mnemonic, a comment and two lines.
; CHECK-LABEL: HBNG_ll_sc_annot:
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM ldaxr\n"
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM stlxr\n"
; CHECK-NOT:   SVC
define i32 @ll_sc(ptr %p, i32 %v) {
  %r = call i32 asm sideeffect "1:\09ldaxr\09$0, [$1] // load-linked, not an svc\0A\09stlxr\09w9, $2, [$1]", "=&r,r,r,~{x9},~{memory}"(ptr %p, i32 %v)
  ret i32 %r
}

; CHECK-LABEL: HBNG_tp_annot:
; CHECK-NEXT:  .ascii "X0 <- Imm(0)\n"
define ptr @tp() {
  %r = call ptr asm "mrs $0, tpidr_el0", "=r"()
  ret ptr %r
}

; compiler-rt's __fe_raise_inexact: read FPSR, set a flag, write it back.
; CHECK-LABEL: HBNG_fpsr_annot:
; CHECK-NEXT:  .ascii "X8 <- Imm(0)\n"
; CHECK-NEXT:  .ascii "X8 <- X8 log Imm(7936)\n"
; CHECK-NEXT:  .ascii "END\n"
define void @fpsr() {
  %r = call i64 asm sideeffect "mrs $0, fpsr", "=r"()
  %s = or i64 %r, 16
  call void asm sideeffect "msr fpsr, $0", "r"(i64 %s)
  ret void
}

; Two outputs: which one an mrs writes is not known here.
; CHECK-LABEL: HBNG_mrs2_annot:
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM mrs\n"
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM mrs\n"
define i64 @mrs2() {
  %r = call { i64, i64 } asm sideeffect "mrs $0, fpcr\0Amrs $1, fpsr", "=r,=r"()
  %a = extractvalue { i64, i64 } %r, 0
  %b = extractvalue { i64, i64 } %r, 1
  %s = add i64 %a, %b
  ret i64 %s
}

; A directive with a comment mentioning svc is neither an instruction nor a
; system call.
; CHECK-LABEL: HBNG_directive_annot:
; CHECK-NEXT:  .ascii "END\n"
define void @directive() {
  call void asm sideeffect ".p2align 2 // svc", ""()
  ret void
}

; Statements separated by ';', the barriers annotating nothing.
; CHECK-LABEL: HBNG_two_annot:
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM fsqrt\n"
; CHECK-NEXT:  .ascii "END\n"
define void @two() {
  call void asm sideeffect "dmb ish; fsqrt d0, d0; isb", "~{d0},~{memory}"()
  ret void
}
