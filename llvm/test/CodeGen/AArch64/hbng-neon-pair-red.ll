; Pairwise NEON operations: output element i from input elements 2i and 2i+1
; of the sources taken one after the other ("per <2e>:<e>", the lower half of
; the destination from the first source, the upper half from the second).
; The pairwise long forms (uaddlp) keep the elements in place, each one twice
; as wide ("per <2e>"), and the accumulating ones (uadalp) also read the old
; destination.
; Reductions, and the scalar pairwise forms: the union of every byte read into
; every byte written, then the bytes above the scalar cleared.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; addp v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_addp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] per 8:4\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- V1[0:15] per 8:4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; umaxp v0.8b, v0.8b, v1.8b
; CHECK-LABEL: HBNG_umaxp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:7] per 2:1\n"
; CHECK-NEXT:  .ascii "V0[4:7] <- V1[0:7] per 2:1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; addp d0, v0.2d ; fmov x0, d0
; CHECK-LABEL: HBNG_addpd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; addv s0, v0.4s ; fmov w0, s0
; CHECK-LABEL: HBNG_addv_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:15]\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; umaxv b0, v0.8b ; fmov w0, s0
; CHECK-LABEL: HBNG_umaxv8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0] <- V0[0:7]\n"
; CHECK-NEXT:  .ascii "V0[1:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; uaddlv h0, v0.16b ; fmov w0, s0
; CHECK-LABEL: HBNG_uaddlv_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:1] <- V0[0:15]\n"
; CHECK-NEXT:  .ascii "V0[2:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; uaddlp v0.8h, v0.16b
; CHECK-LABEL: HBNG_uaddlp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] per 2\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; saddlp v0.2s, v0.4h
; CHECK-LABEL: HBNG_saddlp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 4\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; uadalp v0.2d, v1.4s
; CHECK-LABEL: HBNG_uadalp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] per 8\n"
; CHECK-NEXT:  .ascii "@4\n"
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

define <8 x i16> @uaddlp(<16 x i8> %a) {
entry:
  %r = tail call <8 x i16> @llvm.aarch64.neon.uaddlp.v8i16.v16i8(<16 x i8> %a)
  ret <8 x i16> %r
}

define <2 x i32> @saddlp(<4 x i16> %a) {
entry:
  %r = tail call <2 x i32> @llvm.aarch64.neon.saddlp.v2i32.v4i16(<4 x i16> %a)
  ret <2 x i32> %r
}

define <2 x i64> @uadalp(<2 x i64> %acc, <4 x i32> %a) {
entry:
  %p = tail call <2 x i64> @llvm.aarch64.neon.uaddlp.v2i64.v4i32(<4 x i32> %a)
  %r = add <2 x i64> %acc, %p
  ret <2 x i64> %r
}

declare <4 x i32> @llvm.aarch64.neon.addp.v4i32(<4 x i32>, <4 x i32>)

declare <8 x i8> @llvm.aarch64.neon.umaxp.v8i8(<8 x i8>, <8 x i8>)

declare i64 @llvm.vector.reduce.add.v2i64(<2 x i64>)

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

declare i8 @llvm.vector.reduce.umax.v8i8(<8 x i8>)

declare i32 @llvm.aarch64.neon.uaddlv.i32.v16i8(<16 x i8>)

declare <8 x i16> @llvm.aarch64.neon.uaddlp.v8i16.v16i8(<16 x i8>)

declare <2 x i32> @llvm.aarch64.neon.saddlp.v2i32.v4i16(<4 x i16>)

declare <2 x i64> @llvm.aarch64.neon.uaddlp.v2i64.v4i32(<4 x i32>)
