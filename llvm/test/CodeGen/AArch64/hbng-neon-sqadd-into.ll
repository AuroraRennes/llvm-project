; The saturating adds of a source of the other signedness (suqadd, usqadd) add
; it into the destination: lane i from lane i of the old destination and of the
; source, vector and scalar forms.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; suqadd v0.4s, v1.4s
; CHECK-LABEL: HBNG_suqadd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] per 4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; usqadd v0.8b, v1.8b
; CHECK-LABEL: HBNG_usqadd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] | V1[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fmov d0, x1 ; fmov d1, x0 ; usqadd d1, d0 ; fmov x0, d1
; CHECK-LABEL: HBNG_usqaddd_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- X1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "V1[0:7] <- X0\n"
; CHECK-NEXT:  .ascii "V1[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "V1[0:7] <- V1[0:7] | V0[0:7] per 8\n"
; CHECK-NEXT:  .ascii "V1[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- V1[0:7]\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "END\n"

define <4 x i32> @suqadd(<4 x i32> %acc, <4 x i32> %a) {
entry:
  %r = tail call <4 x i32> @llvm.aarch64.neon.suqadd.v4i32(<4 x i32> %acc, <4 x i32> %a)
  ret <4 x i32> %r
}

define <8 x i8> @usqadd(<8 x i8> %acc, <8 x i8> %a) {
entry:
  %r = tail call <8 x i8> @llvm.aarch64.neon.usqadd.v8i8(<8 x i8> %acc, <8 x i8> %a)
  ret <8 x i8> %r
}

define i64 @usqaddd(i64 %acc, i64 %a) {
entry:
  %r = tail call i64 @llvm.aarch64.neon.usqadd.i64(i64 %acc, i64 %a)
  ret i64 %r
}

declare <4 x i32> @llvm.aarch64.neon.suqadd.v4i32(<4 x i32>, <4 x i32>)

declare <8 x i8> @llvm.aarch64.neon.usqadd.v8i8(<8 x i8>, <8 x i8>)

declare i64 @llvm.aarch64.neon.usqadd.i64(i64, i64)
