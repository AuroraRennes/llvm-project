; AES and SHA (+aes, +sha2): every byte written from every byte read, the old
; destination included when it is a source (aese, aesd, sha1c, sha1su0,
; sha256h, sha256su0, etc.; not aesmc, aesimc, sha1h).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,+aes,+sha2 -hbng -O2 < %s | FileCheck %s

; aese v0.16b, v1.16b
; CHECK-LABEL: HBNG_aese_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; aesmc v0.16b, v0.16b
; CHECK-LABEL: HBNG_aesmc_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; aesd v0.16b, v1.16b ; aesimc v0.16b, v0.16b
; CHECK-LABEL: HBNG_aesdimc_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s0, w0 ; sha1h s0, s0 ; fmov w0, s0
; CHECK-LABEL: HBNG_sha1h_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- X0\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:3] <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "V0[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X0 <- V0[0:3]\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov s2, w0 ; sha1c q0, s2, v1.4s
; CHECK-LABEL: HBNG_sha1c_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V2[0:3] <- X0\n"
; CHECK-NEXT:  .ascii "V2[4:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V2[0:3] | V1[0:15]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; sha1su0 v0.4s, v1.4s, v2.4s
; CHECK-LABEL: HBNG_sha1su0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] | V2[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sha1su1 v0.4s, v1.4s
; CHECK-LABEL: HBNG_sha1su1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sha256h q0, q1, v2.4s
; CHECK-LABEL: HBNG_sha256h_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] | V2[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; sha256su0 v0.4s, v1.4s
; CHECK-LABEL: HBNG_sha256su0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <16 x i8> @aese(<16 x i8> %d, <16 x i8> %k) {
entry:
  %r = tail call <16 x i8> @llvm.aarch64.crypto.aese(<16 x i8> %d, <16 x i8> %k)
  ret <16 x i8> %r
}

define <16 x i8> @aesmc(<16 x i8> %d) {
entry:
  %r = tail call <16 x i8> @llvm.aarch64.crypto.aesmc(<16 x i8> %d)
  ret <16 x i8> %r
}

define <16 x i8> @aesdimc(<16 x i8> %d, <16 x i8> %k) {
entry:
  %a = tail call <16 x i8> @llvm.aarch64.crypto.aesd(<16 x i8> %d, <16 x i8> %k)
  %r = tail call <16 x i8> @llvm.aarch64.crypto.aesimc(<16 x i8> %a)
  ret <16 x i8> %r
}

define i32 @sha1h(i32 %a) {
entry:
  %r = tail call i32 @llvm.aarch64.crypto.sha1h(i32 %a)
  ret i32 %r
}

define <4 x i32> @sha1c(<4 x i32> %abcd, i32 %e, <4 x i32> %wk) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.crypto.sha1c(<4 x i32> %abcd, i32 %e, <4 x i32> %wk)
  ret <4 x i32> %r
}

define <4 x i32> @sha1su0(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.crypto.sha1su0(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c)
  ret <4 x i32> %r
}

define <4 x i32> @sha1su1(<4 x i32> %a, <4 x i32> %b) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.crypto.sha1su1(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

define <4 x i32> @sha256h(<4 x i32> %a, <4 x i32> %b, <4 x i32> %wk) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.crypto.sha256h(<4 x i32> %a, <4 x i32> %b, <4 x i32> %wk)
  ret <4 x i32> %r
}

define <4 x i32> @sha256su0(<4 x i32> %a, <4 x i32> %b) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.crypto.sha256su0(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

declare <16 x i8> @llvm.aarch64.crypto.aese(<16 x i8>, <16 x i8>)

declare <16 x i8> @llvm.aarch64.crypto.aesd(<16 x i8>, <16 x i8>)

declare <16 x i8> @llvm.aarch64.crypto.aesmc(<16 x i8>)

declare <16 x i8> @llvm.aarch64.crypto.aesimc(<16 x i8>)

declare i32 @llvm.aarch64.crypto.sha1h(i32)

declare <4 x i32> @llvm.aarch64.crypto.sha1c(<4 x i32>, i32, <4 x i32>)

declare <4 x i32> @llvm.aarch64.crypto.sha1su0(<4 x i32>, <4 x i32>, <4 x i32>)

declare <4 x i32> @llvm.aarch64.crypto.sha1su1(<4 x i32>, <4 x i32>)

declare <4 x i32> @llvm.aarch64.crypto.sha256h(<4 x i32>, <4 x i32>, <4 x i32>)

declare <4 x i32> @llvm.aarch64.crypto.sha256su0(<4 x i32>, <4 x i32>)
