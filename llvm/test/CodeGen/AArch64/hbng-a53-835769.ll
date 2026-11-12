; The Cortex-A53 erratum 835769 workaround puts a nop between a load and a
; 64-bit multiply-accumulate: an empty group, as for any hint.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,+fix-cortex-a53-835769 -hbng -O2 < %s | FileCheck %s

; str x2, [x18] ; ldr x8, [x2] ; nop ; madd x0, x8, x1, x0
; CHECK-LABEL: HBNG_load_madd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X8 <- M[<X2> .. +8]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- X8 | X1 | X0\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "END\n"

define i64 @load_madd(i64 %a, i64 %b, ptr %c) {
  %l = load i64, ptr %c, align 8
  %m = mul nsw i64 %l, %b
  %r = add nsw i64 %m, %a
  ret i64 %r
}
