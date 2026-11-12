; The polynomial multiplies (pmull, pmull2) widen as umull does: output lane i,
; twice as wide, from element i of both sources ("per <e>:<2e>"), the "2" forms
; from their upper half. The 64-bit forms (.1q, +aes) give one lane of 16 bytes.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,+aes -hbng -O2 < %s | FileCheck %s

; pmull v0.8h, v0.8b, v1.8b
; CHECK-LABEL: HBNG_pmull8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:7] | V1[0:7] per 1:2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; pmull2 v0.8h, v0.16b, v1.16b
; CHECK-LABEL: HBNG_pmull2_8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] | V1[8:15] per 1:2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, x1 ; fmov d1, x0 ; pmull v0.1q, v1.1d, v0.1d
; CHECK-LABEL: HBNG_pmull64_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V1[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V1[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V1[0:7] | V0[0:7] per 8:16\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "END\n"
; pmull2 v0.1q, v0.2d, v1.2d
; CHECK-LABEL: HBNG_pmull2_64_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[8:15] | V1[8:15] per 8:16\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <8 x i16> @pmull8(<8 x i8> %a, <8 x i8> %b) {
entry:
  %r = tail call <8 x i16> @llvm.aarch64.neon.pmull.v8i16(<8 x i8> %a, <8 x i8> %b)
  ret <8 x i16> %r
}

define <8 x i16> @pmull2_8(<16 x i8> %a, <16 x i8> %b) {
entry:
  %ha = shufflevector <16 x i8> %a, <16 x i8> poison, <8 x i32> <i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  %hb = shufflevector <16 x i8> %b, <16 x i8> poison, <8 x i32> <i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  %r = tail call <8 x i16> @llvm.aarch64.neon.pmull.v8i16(<8 x i8> %ha, <8 x i8> %hb)
  ret <8 x i16> %r
}

define <16 x i8> @pmull64(i64 %a, i64 %b) {
entry:
  %r = tail call <16 x i8> @llvm.aarch64.neon.pmull64(i64 %a, i64 %b)
  ret <16 x i8> %r
}

define <16 x i8> @pmull2_64(<2 x i64> %a, <2 x i64> %b) {
entry:
  %ea = extractelement <2 x i64> %a, i64 1
  %eb = extractelement <2 x i64> %b, i64 1
  %r = tail call <16 x i8> @llvm.aarch64.neon.pmull64(i64 %ea, i64 %eb)
  ret <16 x i8> %r
}

declare <8 x i16> @llvm.aarch64.neon.pmull.v8i16(<8 x i8>, <8 x i8>)

declare <16 x i8> @llvm.aarch64.neon.pmull64(i64, i64)
