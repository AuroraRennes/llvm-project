; Scalar FP operations of one source (rounding to an integral value, square
; root): every byte of the result from every byte of the source scalar, the
; bytes above it cleared. The fixed-point conversions (a number of fraction
; bits) are annotated as the integer ones.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; frintp d0, d0
; CHECK-LABEL: HBNG_ceil_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frintm s0, s0
; CHECK-LABEL: HBNG_floorf_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frintx d0, d0
; CHECK-LABEL: HBNG_rint_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fsqrt d0, d0
; CHECK-LABEL: HBNG_sqrt_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frintx d0, d0
; fcvtzs x0, d0
; CHECK-LABEL: HBNG_lrint_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"

; ucvtf d0, x0, #4
; CHECK-LABEL: HBNG_ucvtf_fixed_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtzs w0, d0, #4
; CHECK-LABEL: HBNG_fcvtzs_fixed_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define double @ceil(double %a) {
entry:
  %r = call double @llvm.ceil.f64(double %a)
  ret double %r
}

define float @floorf(float %a) {
entry:
  %r = call float @llvm.floor.f32(float %a)
  ret float %r
}

define double @rint(double %a) {
entry:
  %r = call double @llvm.rint.f64(double %a)
  ret double %r
}

define double @sqrt(double %a) {
entry:
  %r = call double @llvm.sqrt.f64(double %a)
  ret double %r
}

define i64 @lrint(double %a) {
entry:
  %r = call double @llvm.rint.f64(double %a)
  %n = fptosi double %r to i64
  ret i64 %n
}

define double @ucvtf_fixed(i64 %x) {
entry:
  %f = uitofp i64 %x to double
  %r = fdiv double %f, 16.0
  ret double %r
}

define i32 @fcvtzs_fixed(double %x) {
entry:
  %m = fmul double %x, 16.0
  %r = fptosi double %m to i32
  ret i32 %r
}
