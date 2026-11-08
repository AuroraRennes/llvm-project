; Inline asm: nothing for an empty statement (compiler barrier), SVC for a
; system call, and an Unsupported line naming every other instruction instead
; of dropping it. Comments, labels and directives are not instructions.
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
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM mrs\n"
define ptr @tp() {
  %r = call ptr asm "mrs $0, tpidr_el0", "=r"()
  ret ptr %r
}

; A directive with a comment mentioning svc is neither an instruction nor a
; system call.
; CHECK-LABEL: HBNG_directive_annot:
; CHECK-NEXT:  .ascii "END\n"
define void @directive() {
  call void asm sideeffect ".p2align 2 // svc", ""()
  ret void
}

; Statements separated by ';'.
; CHECK-LABEL: HBNG_two_annot:
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM dmb\n"
; CHECK-NEXT:  .ascii "Unsupported: INLINEASM isb\n"
define void @two() {
  call void asm sideeffect "dmb ish; isb", "~{memory}"()
  ret void
}
