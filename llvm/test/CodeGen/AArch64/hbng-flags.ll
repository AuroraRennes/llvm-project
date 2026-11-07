; The condition flags carry no tag: comparisons annotate nothing, flag-setting
; instructions annotate only their destination, conditional selects take both
; data operands but not the condition, the carry is not propagated, branches
; carry no tag into PC and a call writes a clean LR.
; RUN: llc -mtriple=aarch64-linux-musl -O2 < %s | FileCheck %s

; cmp + cset: the result is built from the zero register only, so it is clean.
; CHECK-LABEL: HBNG_is_five_annot:
; CHECK-NEXT:  .ascii "W0 <- WZR cse WZR\n"
; CHECK-NEXT:  .ascii "END\n"
define i32 @is_five(i32 %x) {
  %c = icmp eq i32 %x, 5
  %r = zext i1 %c to i32
  ret i32 %r
}

; fcmp + csel: the selected values only.
; CHECK-LABEL: HBNG_fsel_annot:
; CHECK-NOT:   NZCV
; CHECK:       .ascii "X0 <- X0 cse X1\n"
; CHECK-NEXT:  .ascii "END\n"
define i64 @fsel(double %a, double %b, i64 %x, i64 %y) {
  %c = fcmp olt double %a, %b
  %r = select i1 %c, i64 %x, i64 %y
  ret i64 %r
}

; adds/adc (128-bit add): no carry, no flags.
; CHECK-LABEL: HBNG_add128_annot:
; CHECK-NEXT:  .ascii "X0 <- X0 add X2\n"
; CHECK-NEXT:  .ascii "X1 <- X1 ari X3\n"
; CHECK-NEXT:  .ascii "END\n"
define i128 @add128(i128 %a, i128 %b) {
  %r = add i128 %a, %b
  ret i128 %r
}

; add immediate and a register shift: no flags, no carry.
; CHECK-LABEL: HBNG_addshift_annot:
; CHECK-NEXT:  .ascii "X8 <- X0 add Imm(7)\n"
; CHECK-NEXT:  .ascii "X0 <- X8 shi X1\n"
; CHECK-NEXT:  .ascii "END\n"
define i64 @addshift(i64 %a, i64 %s) {
  %t = add i64 %a, 7
  %r = shl i64 %t, %s
  ret i64 %r
}

; A call: LR is clean, no tag into PC.
; CHECK-LABEL: HBNG_call_annot:
; CHECK:       .ascii "LR <- Imm(0)\n"
; CHECK-NOT:   "PC <-
; CHECK:       .ascii "END\n"
declare void @f()
define void @call() {
  call void @f()
  ret void
}
