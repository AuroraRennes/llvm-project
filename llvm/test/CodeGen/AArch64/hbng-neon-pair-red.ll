; Pairwise NEON operations: output element i from input elements 2i and 2i+1
; of the sources taken one after the other ("pair.<element bytes>").
; Reductions, and the scalar pairwise forms: the union of every byte read into
; every byte written ("red"); a 64-bit source is its 8 low bytes (D).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; addp v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_addp_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 pair.4 Q1\n"
; CHECK-NEXT:  .ascii "END\n"
; umaxp v0.8b, v0.8b, v1.8b
; CHECK-LABEL: HBNG_umaxp_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 pair.1 D1\n"
; CHECK-NEXT:  .ascii "END\n"
; addp d0, v0.2d ; fmov x0, d0
; CHECK-LABEL: HBNG_addpd_annot:
; CHECK-NEXT:  .ascii "D0 <- Q0 red\n"
; CHECK-NEXT:  .ascii "X0 <- D0 mov\n"
; CHECK-NEXT:  .ascii "END\n"
; addv s0, v0.4s ; fmov w0, s0
; CHECK-LABEL: HBNG_addv_annot:
; CHECK-NEXT:  .ascii "S0 <- Q0 red\n"
; CHECK-NEXT:  .ascii "W0 <- S0 mov\n"
; CHECK-NEXT:  .ascii "END\n"
; umaxv b0, v0.8b ; fmov w0, s0
; CHECK-LABEL: HBNG_umaxv8_annot:
; CHECK-NEXT:  .ascii "B0 <- D0 red\n"
; CHECK-NEXT:  .ascii "W0 <- S0 mov\n"
; CHECK-NEXT:  .ascii "END\n"
; uaddlv h0, v0.16b ; fmov w0, s0
; CHECK-LABEL: HBNG_uaddlv_annot:
; CHECK-NEXT:  .ascii "H0 <- Q0 red\n"
; CHECK-NEXT:  .ascii "W0 <- S0 mov\n"
; CHECK-NEXT:  .ascii "END\n"

define <4 x i32> @addp(<4 x i32> %a, <4 x i32> %b) {
entry:
  %vpaddq_v2.i = tail call <4 x i32> @llvm.aarch64.neon.addp.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %vpaddq_v2.i
}

define <8 x i8> @umaxp(<8 x i8> %a, <8 x i8> %b) {
entry:
  %vpmax.i = tail call <8 x i8> @llvm.aarch64.neon.umaxp.v8i8(<8 x i8> %a, <8 x i8> %b)
  ret <8 x i8> %vpmax.i
}

define i64 @addpd(<2 x i64> %a) {
entry:
  %vpaddd_u64.i = tail call i64 @llvm.vector.reduce.add.v2i64(<2 x i64> %a)
  ret i64 %vpaddd_u64.i
}

define i32 @addv(<4 x i32> %a) {
entry:
  %vaddvq_u32.i = tail call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %a)
  ret i32 %vaddvq_u32.i
}

define i8 @umaxv8(<8 x i8> %a) {
entry:
  %vmaxv_u8.i = tail call i8 @llvm.vector.reduce.umax.v8i8(<8 x i8> %a)
  ret i8 %vmaxv_u8.i
}

define i16 @uaddlv(<16 x i8> %a) {
entry:
  %vaddlv.i = tail call i32 @llvm.aarch64.neon.uaddlv.i32.v16i8(<16 x i8> %a)
  %0 = trunc i32 %vaddlv.i to i16
  ret i16 %0
}

declare <4 x i32> @llvm.aarch64.neon.addp.v4i32(<4 x i32>, <4 x i32>)

declare <8 x i8> @llvm.aarch64.neon.umaxp.v8i8(<8 x i8>, <8 x i8>)

declare i64 @llvm.vector.reduce.add.v2i64(<2 x i64>)

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

declare i8 @llvm.vector.reduce.umax.v8i8(<8 x i8>)

declare i32 @llvm.aarch64.neon.uaddlv.i32.v16i8(<16 x i8>)
