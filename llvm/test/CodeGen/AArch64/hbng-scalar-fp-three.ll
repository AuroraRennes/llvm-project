; Scalar FP/SIMD forms of three registers whose opcodes end with their size
; (FMULX64, FRECPS32, FABD32, FCMEQ64, FACGT32), frecpx, and the scalar fcvtxn
; (FCVTXNv1i64, named by its source): one lane of the scalar's size, as the
; vector forms; a comparison's result is clean, its sources "excl". The bytes
; above the scalar are cleared.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; fmulx d0, d0, d1
; CHECK-LABEL: HBNG_fmulxd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frecps s0, s0, s1
; CHECK-LABEL: HBNG_frecpss_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:3] | V1[0:3] per 4\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frsqrts d0, d0, d1
; CHECK-LABEL: HBNG_frsqrtsd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fabd s0, s0, s1
; CHECK-LABEL: HBNG_fabds_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:3] | V1[0:3] per 4\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; frecpx d0, d0
; CHECK-LABEL: HBNG_frecpxd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fcmeq d0, d0, d1
; CHECK-LABEL: HBNG_fcmeqd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- 0 excl V0[0:7] | V1[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; facgt s0, s0, s1 ; fmov w0, s0
; CHECK-LABEL: HBNG_facgts_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- 0 excl V0[0:3] | V1[0:3] per 4\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtxn s0, d0
; CHECK-LABEL: HBNG_fcvtxns_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:7] per 8:4\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define double @fmulxd(double %a, double %b) {
entry:
  %r = tail call double @llvm.aarch64.neon.fmulx.f64(double %a, double %b)
  ret double %r
}

define float @frecpss(float %a, float %b) {
entry:
  %r = tail call float @llvm.aarch64.neon.frecps.f32(float %a, float %b)
  ret float %r
}

define double @frsqrtsd(double %a, double %b) {
entry:
  %r = tail call double @llvm.aarch64.neon.frsqrts.f64(double %a, double %b)
  ret double %r
}

define float @fabds(float %a, float %b) {
entry:
  %r = tail call float @llvm.aarch64.sisd.fabd.f32(float %a, float %b)
  ret float %r
}

define double @frecpxd(double %a) {
entry:
  %r = tail call double @llvm.aarch64.neon.frecpx.f64(double %a)
  ret double %r
}

define <1 x i64> @fcmeqd(<1 x double> %a, <1 x double> %b) {
entry:
  %c = fcmp oeq <1 x double> %a, %b
  %r = sext <1 x i1> %c to <1 x i64>
  ret <1 x i64> %r
}

define i32 @facgts(float %a, float %b) {
entry:
  %r = tail call i32 @llvm.aarch64.neon.facgt.i32.f32(float %a, float %b)
  ret i32 %r
}

define float @fcvtxns(double %a) {
entry:
  %r = tail call float @llvm.aarch64.sisd.fcvtxn(double %a)
  ret float %r
}

declare double @llvm.aarch64.neon.fmulx.f64(double, double)

declare float @llvm.aarch64.neon.frecps.f32(float, float)

declare double @llvm.aarch64.neon.frsqrts.f64(double, double)

declare float @llvm.aarch64.sisd.fabd.f32(float, float)

declare double @llvm.aarch64.neon.frecpx.f64(double)

declare i32 @llvm.aarch64.neon.facgt.i32.f32(float, float)

declare float @llvm.aarch64.sisd.fcvtxn(double)
