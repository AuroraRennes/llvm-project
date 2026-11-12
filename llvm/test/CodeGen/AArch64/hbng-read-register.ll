; A named register read (llvm.read_volatile_register of x20 or d5, as for
; MSVC's __getReg/__getRegFp): the mov or fmov it becomes, from that register.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; mov x0, x20
; CHECK-LABEL: HBNG_read_x20_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- X20\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, d5
; CHECK-LABEL: HBNG_read_d5_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V5[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define i64 @read_x20() {
  %r = call i64 @llvm.read_volatile_register.i64(metadata !0)
  ret i64 %r
}

define double @read_d5() {
  %r = call i64 @llvm.read_volatile_register.i64(metadata !1)
  %d = bitcast i64 %r to double
  ret double %d
}

declare i64 @llvm.read_volatile_register.i64(metadata)

!0 = !{!"x20"}
!1 = !{!"d5"}
