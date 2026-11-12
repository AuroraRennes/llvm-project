; -mharden-sls: the speculation barrier after a ret or br (dsb sy, isb), never
; executed, one empty group per instruction in the block after the branch.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,+harden-sls-retbr -hbng -O2 < %s | FileCheck %s

; add x0, x0, x1
; CHECK-LABEL: HBNG_ret_hardened_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; dsb sy ; isb
; CHECK-LABEL: HBNG_ret_hardened_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; br x0
; CHECK-LABEL: HBNG_br_hardened_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "END\n"
; dsb sy ; isb
; CHECK-LABEL: HBNG_br_hardened_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define i64 @ret_hardened(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

define void @br_hardened(ptr %f) {
  tail call void %f()
  ret void
}
