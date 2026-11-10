; The padding before an aligned block (here a loop header) is not part of the
; block: its label follows the alignment, at the address a branch lands on,
; so that the block's first group is its first instruction.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 -align-loops=16 < %s | FileCheck %s

; CHECK-LABEL: HBNG_loop:
; CHECK-NEXT:    mov x8, xzr
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_1:
; CHECK:       HBNG_loop_n_1:
; CHECK-NEXT:    stp x8, x0, [x18]
; CHECK-NEXT:    str x8, [x0, x8, lsl #3]
; CHECK-LABEL: HBNG_loop_n_1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X0\n"
; CHECK-NEXT:  .ascii "send X8\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "M[<X0> + ext(<X8>, uxtx, 3) .. +8] <- X8\n"

define void @loop(ptr %p, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %a = getelementptr i64, ptr %p, i64 %i
  store i64 %i, ptr %a
  %i.next = add i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
