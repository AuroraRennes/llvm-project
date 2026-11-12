; The widening multiplies by element (smull, umlal2, sqdmull with v.s[i]): output
; lane i, twice as wide, from element i of the narrow source (its upper half
; for the "2" forms), the same element of the indexed register in every lane,
; and the old lane for the accumulating forms; one line per lane. The scalar
; forms (sqdmull d0, s1, s2, sqdmlal d0, s1, v2.s[2]) read their elements half
; as wide, the bytes above the scalar cleared.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; smull v0.4s, v0.4h, v1.h[1]
; CHECK-LABEL: HBNG_smull_lane_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:1] | V1[2:3]\n"
; CHECK-NEXT:  .ascii "V0[4:7] <- V0[2:3] | V1[2:3]\n"
; CHECK-NEXT:  .ascii "V0[8:11] <- V0[4:5] | V1[2:3]\n"
; CHECK-NEXT:  .ascii "V0[12:15] <- V0[6:7] | V1[2:3]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; umlal2 v0.2d, v1.4s, v2.s[3]
; CHECK-LABEL: HBNG_umlal2_lane_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[8:11] | V2[12:15]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V0[8:15] | V1[12:15] | V2[12:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sqdmull v0.2d, v0.2s, v1.s[1]
; CHECK-LABEL: HBNG_sqdmull_lane_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:3] | V1[4:7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V0[4:7] | V1[4:7]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s0, w1 ; fmov s1, w0 ; sqdmull d0, s1, s0 ; fmov x0, d0
; CHECK-LABEL: HBNG_sqdmull_scalar_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- X1\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V1[0:3] <- X0\n"
; CHECK-NEXT:  .ascii "V1[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V1[0:3] | V0[0:3]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s0, w2 ; fmov s1, w1 ; fmov d2, x0 ; sqdmlal d2, s1, s0 ; fmov x0, d2
; CHECK-LABEL: HBNG_sqdmlal_scalar_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- X2\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V1[0:3] <- X1\n"
; CHECK-NEXT:  .ascii "V1[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "V2[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V2[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "V2[0:7] <- V2[0:7] | V1[0:3] | V0[0:3]\n"
; CHECK-NEXT:  .ascii "V2[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "X0 <- V2[0:7]\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s1, w1 ; fmov d2, x0 ; sqdmlal d2, s1, v0.s[2] ; fmov x0, d2
; CHECK-LABEL: HBNG_sqdmlal_scalar_lane_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V1[0:3] <- X1\n"
; CHECK-NEXT:  .ascii "V1[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V2[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V2[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "V2[0:7] <- V2[0:7] | V1[0:3] | V0[8:11]\n"
; CHECK-NEXT:  .ascii "V2[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- V2[0:7]\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "END\n"

define <4 x i32> @smull_lane(<4 x i16> %a, <4 x i16> %b) {
entry:
  %d = shufflevector <4 x i16> %b, <4 x i16> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %r = tail call <4 x i32> @llvm.aarch64.neon.smull.v4i32(<4 x i16> %a, <4 x i16> %d)
  ret <4 x i32> %r
}

define <2 x i64> @umlal2_lane(<2 x i64> %acc, <4 x i32> %a, <4 x i32> %b) {
entry:
  %h = shufflevector <4 x i32> %a, <4 x i32> poison, <2 x i32> <i32 2, i32 3>
  %d = shufflevector <4 x i32> %b, <4 x i32> poison, <2 x i32> <i32 3, i32 3>
  %m = tail call <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32> %h, <2 x i32> %d)
  %r = add <2 x i64> %acc, %m
  ret <2 x i64> %r
}

define <2 x i64> @sqdmull_lane(<2 x i32> %a, <2 x i32> %b) {
entry:
  %d = shufflevector <2 x i32> %b, <2 x i32> poison, <2 x i32> <i32 1, i32 1>
  %r = tail call <2 x i64> @llvm.aarch64.neon.sqdmull.v2i64(<2 x i32> %a, <2 x i32> %d)
  ret <2 x i64> %r
}

define i64 @sqdmull_scalar(i32 %a, i32 %b) {
entry:
  %r = tail call i64 @llvm.aarch64.neon.sqdmulls.scalar(i32 %a, i32 %b)
  ret i64 %r
}

define i64 @sqdmlal_scalar(i64 %acc, i32 %a, i32 %b) {
entry:
  %m = tail call i64 @llvm.aarch64.neon.sqdmulls.scalar(i32 %a, i32 %b)
  %r = tail call i64 @llvm.aarch64.neon.sqadd.i64(i64 %acc, i64 %m)
  ret i64 %r
}

define i64 @sqdmlal_scalar_lane(i64 %acc, i32 %a, <4 x i32> %b) {
entry:
  %e = extractelement <4 x i32> %b, i64 2
  %m = tail call i64 @llvm.aarch64.neon.sqdmulls.scalar(i32 %a, i32 %e)
  %r = tail call i64 @llvm.aarch64.neon.sqadd.i64(i64 %acc, i64 %m)
  ret i64 %r
}

declare <4 x i32> @llvm.aarch64.neon.smull.v4i32(<4 x i16>, <4 x i16>)

declare <2 x i64> @llvm.aarch64.neon.umull.v2i64(<2 x i32>, <2 x i32>)

declare <2 x i64> @llvm.aarch64.neon.sqdmull.v2i64(<2 x i32>, <2 x i32>)

declare i64 @llvm.aarch64.neon.sqdmulls.scalar(i32, i32)

declare i64 @llvm.aarch64.neon.sqadd.i64(i64, i64)
