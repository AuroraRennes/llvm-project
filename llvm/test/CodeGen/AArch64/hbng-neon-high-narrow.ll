; The narrowing high-half adds and subtracts (addhn, raddhn, subhn, rsubhn),
; whose opcodes name the sources' arrangement before the destination's
; (ADDHNv4i32_v4i16): output lane i from lane i of both sources, twice as wide
; ("per <2e>:<e>"), the bytes above cleared, or, for the "2" forms, into the
; upper half of the destination, the lower one kept.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; addhn v0.4h, v0.4s, v1.4s
; CHECK-LABEL: HBNG_addhn_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] | V1[0:15] per 4:2\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; raddhn2 v0.16b, v1.8h, v2.8h
; CHECK-LABEL: HBNG_raddhn2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V1[0:15] | V2[0:15] per 2:1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; subhn v0.2s, v0.2d, v1.2d
; CHECK-LABEL: HBNG_subhn_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] | V1[0:15] per 8:4\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; rsubhn2 v0.4s, v1.2d, v2.2d
; CHECK-LABEL: HBNG_rsubhn2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V1[0:15] | V2[0:15] per 8:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <4 x i16> @addhn(<4 x i32> %a, <4 x i32> %b) {
entry:
  %r = tail call <4 x i16> @llvm.aarch64.neon.addhn.v4i16(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i16> %r
}

define <16 x i8> @raddhn2(<8 x i8> %lo, <8 x i16> %a, <8 x i16> %b) {
entry:
  %h = tail call <8 x i8> @llvm.aarch64.neon.raddhn.v8i8(<8 x i16> %a, <8 x i16> %b)
  %r = shufflevector <8 x i8> %lo, <8 x i8> %h, <16 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  ret <16 x i8> %r
}

define <2 x i32> @subhn(<2 x i64> %a, <2 x i64> %b) {
entry:
  %d = sub <2 x i64> %a, %b
  %s = lshr <2 x i64> %d, <i64 32, i64 32>
  %r = trunc <2 x i64> %s to <2 x i32>
  ret <2 x i32> %r
}

define <4 x i32> @rsubhn2(<2 x i32> %lo, <2 x i64> %a, <2 x i64> %b) {
entry:
  %h = tail call <2 x i32> @llvm.aarch64.neon.rsubhn.v2i32(<2 x i64> %a, <2 x i64> %b)
  %r = shufflevector <2 x i32> %lo, <2 x i32> %h, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  ret <4 x i32> %r
}

declare <4 x i16> @llvm.aarch64.neon.addhn.v4i16(<4 x i32>, <4 x i32>)

declare <8 x i8> @llvm.aarch64.neon.raddhn.v8i8(<8 x i16>, <8 x i16>)

declare <2 x i32> @llvm.aarch64.neon.rsubhn.v2i32(<2 x i64>, <2 x i64>)
