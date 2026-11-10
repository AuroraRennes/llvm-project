; Widening and narrowing NEON operations: union per element, with different
; input and output element sizes ("per <in>:<out>", in bytes). The "2" forms
; of widening read the upper half of their sources (V<n>[8:15]); those of
; narrowing write the upper half of the destination and keep the lower one.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; ushll v0.4s, v0.4h, #3
; CHECK-LABEL: HBNG_ushll_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] per 2:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; ushll2 v0.4s, v0.8h, #3
; CHECK-LABEL: HBNG_ushll2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] per 2:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; ushll v0.8h, v0.8b, #0
; CHECK-LABEL: HBNG_uxtl_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] per 1:2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; ushll2 v0.8h, v0.16b, #0
; CHECK-LABEL: HBNG_uxtl2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] per 1:2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sshll v0.2d, v0.2s, #0
; CHECK-LABEL: HBNG_sxtl_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] per 4:8\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; shll v0.4s, v0.4h, #16
; CHECK-LABEL: HBNG_shll_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] per 2:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; shll2 v0.4s, v0.8h, #16
; CHECK-LABEL: HBNG_shll2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] per 2:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; umull v0.2d, v0.2s, v1.2s
; CHECK-LABEL: HBNG_umull_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] | V1[0:7] per 4:8\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; umull2 v0.2d, v0.4s, v1.4s
; CHECK-LABEL: HBNG_umull2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] | V1[8:15] per 4:8\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; saddl v0.8h, v1.8b, v0.8b
; CHECK-LABEL: HBNG_saddl_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V1[0:7] | V0[0:7] per 1:2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; xtn v0.8b, v0.8h
; CHECK-LABEL: HBNG_xtn_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] per 2:1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; xtn v0.4h, v0.4s
; CHECK-LABEL: HBNG_xtn4h_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] per 4:2\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; xtn2 v0.16b, v1.8h
; CHECK-LABEL: HBNG_xtn2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V1[0:15] per 2:1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; shrn v0.8b, v0.8h, #3
; CHECK-LABEL: HBNG_shrn_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] per 2:1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sqxtn v0.4h, v0.4s
; CHECK-LABEL: HBNG_sqxtn_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] per 4:2\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

; The accumulating long forms read a destination of the output element size
; with sources of the input size; not described yet, they stay unsupported.
; CHECK-LABEL: HBNG_umlal_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "unsupported \"UMLALv2i32_v2i64\"\n"

define <4 x i32> @ushll(<4 x i16> %a) {
entry:
  %0 = zext <4 x i16> %a to <4 x i32>
  %vshll_n = shl nuw nsw <4 x i32> %0, splat (i32 3)
  ret <4 x i32> %vshll_n
}

define <4 x i32> @ushll2(<8 x i16> %a) {
entry:
  %shuffle.i = shufflevector <8 x i16> %a, <8 x i16> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %0 = zext <4 x i16> %shuffle.i to <4 x i32>
  %vshll_n = shl nuw nsw <4 x i32> %0, splat (i32 3)
  ret <4 x i32> %vshll_n
}

define <8 x i16> @uxtl(<8 x i8> %a) {
entry:
  %vmovl.i = zext <8 x i8> %a to <8 x i16>
  ret <8 x i16> %vmovl.i
}

define <8 x i16> @uxtl2(<16 x i8> %a) {
entry:
  %shuffle.i = shufflevector <16 x i8> %a, <16 x i8> poison, <8 x i32> <i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  %0 = zext <8 x i8> %shuffle.i to <8 x i16>
  ret <8 x i16> %0
}

define <2 x i64> @sxtl(<2 x i32> %a) {
entry:
  %vmovl.i = sext <2 x i32> %a to <2 x i64>
  ret <2 x i64> %vmovl.i
}

define <4 x i32> @shll(<4 x i16> %a) {
entry:
  %0 = zext <4 x i16> %a to <4 x i32>
  %vshll_n = shl nuw <4 x i32> %0, splat (i32 16)
  ret <4 x i32> %vshll_n
}

define <4 x i32> @shll2(<8 x i16> %a) {
entry:
  %shuffle.i = shufflevector <8 x i16> %a, <8 x i16> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %0 = zext <4 x i16> %shuffle.i to <4 x i32>
  %vshll_n = shl nuw <4 x i32> %0, splat (i32 16)
  ret <4 x i32> %vshll_n
}

define <2 x i64> @umull(<2 x i32> %a, <2 x i32> %b) {
entry:
  %vmull2.i = tail call <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32> %a, <2 x i32> %b)
  ret <2 x i64> %vmull2.i
}

define <2 x i64> @umull2(<4 x i32> %a, <4 x i32> %b) {
entry:
  %shuffle.i1 = shufflevector <4 x i32> %a, <4 x i32> poison, <2 x i32> <i32 2, i32 3>
  %shuffle.i = shufflevector <4 x i32> %b, <4 x i32> poison, <2 x i32> <i32 2, i32 3>
  %vmull2.i.i = tail call <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32> %shuffle.i1, <2 x i32> %shuffle.i)
  ret <2 x i64> %vmull2.i.i
}

define <8 x i16> @saddl(<8 x i8> %a, <8 x i8> %b) {
entry:
  %vmovl.i1 = sext <8 x i8> %a to <8 x i16>
  %vmovl.i = sext <8 x i8> %b to <8 x i16>
  %add.i = add nsw <8 x i16> %vmovl.i, %vmovl.i1
  ret <8 x i16> %add.i
}

define <8 x i8> @xtn(<8 x i16> %a) {
entry:
  %vmovn.i = trunc <8 x i16> %a to <8 x i8>
  ret <8 x i8> %vmovn.i
}

define <4 x i16> @xtn4h(<4 x i32> %a) {
entry:
  %vmovn.i = trunc <4 x i32> %a to <4 x i16>
  ret <4 x i16> %vmovn.i
}

define <16 x i8> @xtn2(<8 x i8> %l, <8 x i16> %a) {
entry:
  %vmovn.i.i = trunc <8 x i16> %a to <8 x i8>
  %shuffle.i = shufflevector <8 x i8> %l, <8 x i8> %vmovn.i.i, <16 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  ret <16 x i8> %shuffle.i
}

define <8 x i8> @shrn(<8 x i16> %a) {
entry:
  %0 = lshr <8 x i16> %a, splat (i16 3)
  %vshrn_n = trunc <8 x i16> %0 to <8 x i8>
  ret <8 x i8> %vshrn_n
}

define <4 x i16> @sqxtn(<4 x i32> %a) {
entry:
  %vqmovn_v1.i = tail call <4 x i16> @llvm.aarch64.neon.sqxtn.v4i16(<4 x i32> %a)
  ret <4 x i16> %vqmovn_v1.i
}

declare <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32>, <2 x i32>)

declare <4 x i16> @llvm.aarch64.neon.sqxtn.v4i16(<4 x i32>)

define <2 x i64> @umlal(<2 x i64> %acc, <2 x i32> %a, <2 x i32> %b) {
  %m = call <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32> %a, <2 x i32> %b)
  %r = add <2 x i64> %acc, %m
  ret <2 x i64> %r
}
