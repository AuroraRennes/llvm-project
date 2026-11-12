; Scalar FP/SIMD forms with an immediate, their size as a suffix of the opcode
; (SHLd, SQSHRNs, SCVTFd): one lane of the scalar's size, as the vector forms.
; The shifts and fixed-point conversions read the scalar (and the old
; destination when they accumulate or insert: ssra, sli); the saturating
; narrowing shifts write a scalar half as wide ("per 8:4"). The bytes above the
; scalar are cleared.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; shl d0, d0, #3
; CHECK-LABEL: HBNG_shld_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; ssra d0, d1, #5
; CHECK-LABEL: HBNG_ssrad_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sli d0, d1, #7
; CHECK-LABEL: HBNG_slid_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, x0 ; sqshlu d0, d0, #3 ; fmov x0, d0
; CHECK-LABEL: HBNG_sqshlud_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, x0 ; sqshrn s0, d0, #5 ; fmov w0, s0
; CHECK-LABEL: HBNG_sqshrns_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:7] per 8:4\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, x0 ; scvtf d0, d0, #3
; CHECK-LABEL: HBNG_scvtfd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtzs d0, d0, #3 ; fmov x0, d0
; CHECK-LABEL: HBNG_fcvtzsd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"

define <1 x i64> @shld(<1 x i64> %a) {
entry:
  %r = shl <1 x i64> %a, <i64 3>
  ret <1 x i64> %r
}

define <1 x i64> @ssrad(<1 x i64> %acc, <1 x i64> %a) {
entry:
  %s = ashr <1 x i64> %a, <i64 5>
  %r = add <1 x i64> %acc, %s
  ret <1 x i64> %r
}

define <1 x i64> @slid(<1 x i64> %a, <1 x i64> %b) {
entry:
  %r = tail call <1 x i64> @llvm.aarch64.neon.vsli.v1i64(<1 x i64> %a, <1 x i64> %b, i32 7)
  ret <1 x i64> %r
}

define i64 @sqshlud(i64 %a) {
entry:
  %r = tail call i64 @llvm.aarch64.neon.sqshlu.i64(i64 %a, i64 3)
  ret i64 %r
}

define i32 @sqshrns(i64 %a) {
entry:
  %r = tail call i32 @llvm.aarch64.neon.sqshrn.i32(i64 %a, i32 5)
  ret i32 %r
}

define double @scvtfd(i64 %a) {
entry:
  %r = tail call double @llvm.aarch64.neon.vcvtfxs2fp.f64.i64(i64 %a, i32 3)
  ret double %r
}

define i64 @fcvtzsd(double %a) {
entry:
  %r = tail call i64 @llvm.aarch64.neon.vcvtfp2fxs.i64.f64(double %a, i32 3)
  ret i64 %r
}

declare <1 x i64> @llvm.aarch64.neon.vsli.v1i64(<1 x i64>, <1 x i64>, i32)

declare i64 @llvm.aarch64.neon.sqshlu.i64(i64, i64)

declare i32 @llvm.aarch64.neon.sqshrn.i32(i64, i32)

declare double @llvm.aarch64.neon.vcvtfxs2fp.f64.i64(i64, i32)

declare i64 @llvm.aarch64.neon.vcvtfp2fxs.i64.f64(double, i32)
