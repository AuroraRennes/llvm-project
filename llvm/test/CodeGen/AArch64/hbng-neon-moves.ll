; Moves between registers, lanes and immediates. A lane insert writes
; V<n>[lo:hi] and keeps the other bytes; a lane read names its bytes. An X/W
; register has one tag: reading a V register into it takes the union of the
; bytes read, and writing it to a V register gives its tag to every byte
; written (dup). Scalar and 64-bit destinations clear the bytes above, on
; their own line. An immediate move is a clear.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; mov v0.s[2], w0
; CHECK-LABEL: HBNG_insgpr_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[8:11] <- X0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov v0.b[5], w0
; CHECK-LABEL: HBNG_insgpr8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[5] <- X0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov v0.s[0], v1.s[3]
; CHECK-LABEL: HBNG_inslane_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V1[12:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov w0, v0.s[1]
; CHECK-LABEL: HBNG_umov_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[4:7]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; umov w0, v0.b[0]
; CHECK-LABEL: HBNG_umov0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov x0, d0
; CHECK-LABEL: HBNG_umov64_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; smov x0, v0.h[5]
; CHECK-LABEL: HBNG_smov_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[10:11]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov s0, v0.s[2]
; CHECK-LABEL: HBNG_dups_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[8:11]\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; dup v0.16b, w0
; CHECK-LABEL: HBNG_dupgpr_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- X0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; dup v0.2s, w0
; CHECK-LABEL: HBNG_dupgpr2s_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s0, w0
; CHECK-LABEL: HBNG_fmovws_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- X0\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov x0, d0
; CHECK-LABEL: HBNG_fmovxd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov v0.d[1], x0
; CHECK-LABEL: HBNG_fmovhigh_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- X0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mov x0, v0.d[1]
; CHECK-LABEL: HBNG_fmovfromhigh_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- V0[8:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; movi v0.8b, #90
; CHECK-LABEL: HBNG_movi8b_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; movi v0.4s, #18, lsl #8
; CHECK-LABEL: HBNG_movi4s_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; movi v0.2d, #0000000000000000
; CHECK-LABEL: HBNG_movi2d_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov v0.4s, #1.00000000
; CHECK-LABEL: HBNG_fmovv_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <4 x i32> @insgpr(<4 x i32> %v, i32 %x) {
entry:
  %vset_lane = insertelement <4 x i32> %v, i32 %x, i64 2
  ret <4 x i32> %vset_lane
}

define <16 x i8> @insgpr8(<16 x i8> %v, i8 %x) {
entry:
  %vset_lane = insertelement <16 x i8> %v, i8 %x, i64 5
  ret <16 x i8> %vset_lane
}

define <4 x i32> @inslane(<4 x i32> %a, <4 x i32> %b) {
entry:
  %vset_lane = shufflevector <4 x i32> %a, <4 x i32> %b, <4 x i32> <i32 7, i32 1, i32 2, i32 3>
  ret <4 x i32> %vset_lane
}

define i32 @umov(<4 x i32> %a) {
entry:
  %vgetq_lane = extractelement <4 x i32> %a, i64 1
  ret i32 %vgetq_lane
}

define i8 @umov0(<16 x i8> %a) {
entry:
  %vgetq_lane = extractelement <16 x i8> %a, i64 0
  ret i8 %vgetq_lane
}

define i64 @umov64(<2 x i64> %a) {
entry:
  %vgetq_lane = extractelement <2 x i64> %a, i64 0
  ret i64 %vgetq_lane
}

define i64 @smov(<8 x i16> %a) {
entry:
  %vgetq_lane = extractelement <8 x i16> %a, i64 5
  %conv = sext i16 %vgetq_lane to i64
  ret i64 %conv
}

define float @dups(<4 x float> %a) {
entry:
  %vgetq_lane = extractelement <4 x float> %a, i64 2
  ret float %vgetq_lane
}

define <16 x i8> @dupgpr(i8 %a) {
entry:
  %vecinit.i = insertelement <16 x i8> poison, i8 %a, i64 0
  %vecinit15.i = shufflevector <16 x i8> %vecinit.i, <16 x i8> poison, <16 x i32> zeroinitializer
  ret <16 x i8> %vecinit15.i
}

define <2 x i32> @dupgpr2s(i32 %a) {
entry:
  %vecinit.i = insertelement <2 x i32> poison, i32 %a, i64 0
  %vecinit1.i = shufflevector <2 x i32> %vecinit.i, <2 x i32> poison, <2 x i32> zeroinitializer
  ret <2 x i32> %vecinit1.i
}

define float @fmovws(i32 %a) {
entry:
  %0 = bitcast i32 %a to float
  ret float %0
}

define i64 @fmovxd(double %a) {
entry:
  %0 = bitcast double %a to i64
  ret i64 %0
}

define <2 x i64> @fmovhigh(<2 x i64> %v, i64 %x) {
entry:
  %vset_lane = insertelement <2 x i64> %v, i64 %x, i64 1
  ret <2 x i64> %vset_lane
}

define i64 @fmovfromhigh(<2 x i64> %v) {
entry:
  %vgetq_lane = extractelement <2 x i64> %v, i64 1
  ret i64 %vgetq_lane
}

define <8 x i8> @movi8b() {
entry:
  ret <8 x i8> splat (i8 90)
}

define <4 x i32> @movi4s() {
entry:
  ret <4 x i32> splat (i32 4608)
}

define <2 x i64> @movi2d() {
entry:
  ret <2 x i64> zeroinitializer
}

define <4 x float> @fmovv() {
entry:
  ret <4 x float> splat (float 1.000000e+00)
}
