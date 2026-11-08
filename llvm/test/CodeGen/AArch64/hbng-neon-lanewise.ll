; Lane-wise NEON arithmetic, shifts, bit counts and conversions: each byte of
; an element gets the union of the tags of the bytes of that element in every
; source (a carry or a product mixes the bytes of a lane, lanes stay apart),
; written <op>.<element bytes>. A destination read by the instruction (mls,
; usra, fmla) is a source. A by-element operand is its lane, V<n>[lo:hi], used
; for every element. Scalar FP keeps the operator without element size: the
; register is one element.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; add v0.4s, v1.4s, v0.4s
; CHECK-LABEL: HBNG_add4_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q1 add.4 Q0\n"
; CHECK-NEXT:  .ascii "END\n"
; add v0.8b, v1.8b, v0.8b
; CHECK-LABEL: HBNG_add8b_annot:
; CHECK-NEXT:  .ascii "D0 <- D1 add.1 D0\n"
; CHECK-NEXT:  .ascii "END\n"
; sub v0.2s, v0.2s, v1.2s
; CHECK-LABEL: HBNG_sub2_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 sub.4 D1\n"
; CHECK-NEXT:  .ascii "END\n"
; add d0, d1, d0
; CHECK-LABEL: HBNG_add1d_annot:
; CHECK-NEXT:  .ascii "D0 <- D1 add.8 D0\n"
; CHECK-NEXT:  .ascii "END\n"
; mls v0.2s, v2.2s, v1.2s
; CHECK-LABEL: HBNG_mls_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 ari.4 D2 ari.4 D1\n"
; CHECK-NEXT:  .ascii "END\n"
; mul v0.2s, v0.2s, v1.s[1]
; CHECK-LABEL: HBNG_mulidx_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 ari.4 V1[4:7]\n"
; CHECK-NEXT:  .ascii "END\n"
; fmla s0, s1, v2.s[1]
; CHECK-LABEL: HBNG_fmla1_annot:
; CHECK-NEXT:  .ascii "S0 <- S0 ari.4 S1 ari.4 V2[4:7]\n"
; CHECK-NEXT:  .ascii "END\n"
; smax v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_smax_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 ari.4 Q1\n"
; CHECK-NEXT:  .ascii "END\n"
; neg v0.4s, v0.4s
; CHECK-LABEL: HBNG_neg_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 ari.4\n"
; CHECK-NEXT:  .ascii "END\n"
; fadd v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_fadd_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 add.4 Q1\n"
; CHECK-NEXT:  .ascii "END\n"
; shl v0.8b, v0.8b, #2
; CHECK-LABEL: HBNG_shl8b_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 shi.1 Imm(2)\n"
; CHECK-NEXT:  .ascii "END\n"
; ushr v0.4s, v0.4s, #3
; CHECK-LABEL: HBNG_ushr_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 shi.4 Imm(3)\n"
; CHECK-NEXT:  .ascii "END\n"
; ushr v0.2d, v0.2d, #7
; CHECK-LABEL: HBNG_ushr2d_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 shi.8 Imm(7)\n"
; CHECK-NEXT:  .ascii "END\n"
; usra v0.4s, v1.4s, #3
; CHECK-LABEL: HBNG_usra_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 shi.4 Q1 shi.4 Imm(3)\n"
; CHECK-NEXT:  .ascii "END\n"
; ushl v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_ushl_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 shi.4 Q1\n"
; CHECK-NEXT:  .ascii "END\n"
; cnt v0.16b, v0.16b
; CHECK-LABEL: HBNG_cnt_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit.1\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtzs v0.4s, v0.4s
; CHECK-LABEL: HBNG_cvt_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 cvt.4\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtzs w0, s0
; CHECK-LABEL: HBNG_fcvtzs1_annot:
; CHECK-NEXT:  .ascii "W0 <- S0 cvt\n"
; CHECK-NEXT:  .ascii "END\n"
; fmaxnm s0, s0, s1
; CHECK-LABEL: HBNG_fmaxnm_annot:
; CHECK-NEXT:  .ascii "S0 <- S0 ari S1\n"
; CHECK-NEXT:  .ascii "END\n"
; fcvtzu x0, d0
; CHECK-LABEL: HBNG_f2ul_annot:
; CHECK-NEXT:  .ascii "X0 <- D0 cvt\n"
; fcvtas x0, d0
; CHECK-LABEL: HBNG_lround_annot:
; CHECK-NEXT:  .ascii "X0 <- D0 cvt\n"

define <4 x i32> @add4(<4 x i32> %a, <4 x i32> %b) {
entry:
  %add.i = add <4 x i32> %b, %a
  ret <4 x i32> %add.i
}

define <8 x i8> @add8b(<8 x i8> %a, <8 x i8> %b) {
entry:
  %add.i = add <8 x i8> %b, %a
  ret <8 x i8> %add.i
}

define <2 x i32> @sub2(<2 x i32> %a, <2 x i32> %b) {
entry:
  %sub.i = sub <2 x i32> %a, %b
  ret <2 x i32> %sub.i
}

define <1 x i64> @add1d(<1 x i64> %a, <1 x i64> %b) {
entry:
  %add.i = add <1 x i64> %b, %a
  ret <1 x i64> %add.i
}

define <2 x i32> @mls(<2 x i32> %d, <2 x i32> %a, <2 x i32> %b) {
entry:
  %mul.i = mul <2 x i32> %b, %a
  %sub.i = sub <2 x i32> %d, %mul.i
  ret <2 x i32> %sub.i
}

define <2 x i32> @mulidx(<2 x i32> %a, <4 x i32> %b) {
entry:
  %lane = shufflevector <4 x i32> %b, <4 x i32> poison, <2 x i32> <i32 1, i32 1>
  %mul = mul <2 x i32> %lane, %a
  ret <2 x i32> %mul
}

define float @fmla1(float %d, float %a, <4 x float> %b) {
entry:
  %extract = extractelement <4 x float> %b, i64 1
  %0 = tail call float @llvm.fma.f32(float %a, float %extract, float %d)
  ret float %0
}

declare float @llvm.fma.f32(float, float, float)

define <4 x i32> @smax(<4 x i32> %a, <4 x i32> %b) {
entry:
  %vmax2.i = tail call <4 x i32> @llvm.aarch64.neon.smax.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %vmax2.i
}

define <4 x i32> @neg(<4 x i32> %a) {
entry:
  %sub.i = sub <4 x i32> zeroinitializer, %a
  ret <4 x i32> %sub.i
}

define <4 x float> @fadd(<4 x float> %a, <4 x float> %b) {
entry:
  %add.i = fadd <4 x float> %a, %b
  ret <4 x float> %add.i
}

define <8 x i8> @shl8b(<8 x i8> %a) {
entry:
  %vshl_n = shl <8 x i8> %a, splat (i8 2)
  ret <8 x i8> %vshl_n
}

define <4 x i32> @ushr(<4 x i32> %a) {
entry:
  %vshr_n = lshr <4 x i32> %a, splat (i32 3)
  ret <4 x i32> %vshr_n
}

define <2 x i64> @ushr2d(<2 x i64> %a) {
entry:
  %vshr_n = lshr <2 x i64> %a, splat (i64 7)
  ret <2 x i64> %vshr_n
}

define <4 x i32> @usra(<4 x i32> %d, <4 x i32> %a) {
entry:
  %vsra_n = lshr <4 x i32> %a, splat (i32 3)
  %0 = add <4 x i32> %vsra_n, %d
  ret <4 x i32> %0
}

define <4 x i32> @ushl(<4 x i32> %a, <4 x i32> %b) {
entry:
  %vshlq_v2.i = tail call <4 x i32> @llvm.aarch64.neon.ushl.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %vshlq_v2.i
}

define <16 x i8> @cnt(<16 x i8> %a) {
entry:
  %vcntq_v.i = tail call <16 x i8> @llvm.ctpop.v16i8(<16 x i8> %a)
  ret <16 x i8> %vcntq_v.i
}

define <4 x i32> @cvt(<4 x float> %a) {
entry:
  %vcvtz.i = tail call <4 x i32> @llvm.fptosi.sat.v4i32.v4f32(<4 x float> %a)
  ret <4 x i32> %vcvtz.i
}

define i32 @fcvtzs1(float %a) {
entry:
  %vcvts_s32_f32.i = tail call i32 @llvm.fptosi.sat.i32.f32(float %a)
  ret i32 %vcvts_s32_f32.i
}

define float @fmaxnm(float %a, float %b) {
entry:
  %0 = tail call nsz float @llvm.maxnum.f32(float %a, float %b)
  ret float %0
}

declare float @llvm.maxnum.f32(float, float)

declare <4 x i32> @llvm.aarch64.neon.smax.v4i32(<4 x i32>, <4 x i32>)

declare <4 x i32> @llvm.aarch64.neon.ushl.v4i32(<4 x i32>, <4 x i32>)

declare <16 x i8> @llvm.ctpop.v16i8(<16 x i8>)

declare <4 x i32> @llvm.fptosi.sat.v4i32.v4f32(<4 x float>)

declare i32 @llvm.fptosi.sat.i32.f32(float)

define i64 @f2ul(double %d) {
  %r = fptoui double %d to i64
  ret i64 %r
}

define i64 @lround(double %d) {
  %r = call i64 @llvm.lround.i64.f64(double %d)
  ret i64 %r
}
