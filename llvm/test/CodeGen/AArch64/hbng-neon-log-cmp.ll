; Bitwise NEON operations move exact bytes: byte k of the destination from
; byte k of each source ("per 1"). The immediate forms of orr/bic read their
; destination. Comparisons give clean results, the operands compared listed
; under "excl" (no tag on a comparison result, as for the condition flags).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; and v0.16b, v1.16b, v0.16b
; CHECK-LABEL: HBNG_and16_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V1[0:15] | V0[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; eor v0.8b, v1.8b, v0.8b
; CHECK-LABEL: HBNG_eor8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V1[0:7] | V0[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; bic v0.16b, v0.16b, v1.16b
; CHECK-LABEL: HBNG_bic16_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mvn v0.8b, v0.8b
; CHECK-LABEL: HBNG_not8_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; bic v0.4s, #255, lsl #8
; CHECK-LABEL: HBNG_bicimm_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; orr v0.4h, #3
; CHECK-LABEL: HBNG_orrimm_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; cmhs v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_cmhs_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0 excl V0[0:15] | V1[0:15] per 4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; cmeq v0.4s, v0.4s, #0
; CHECK-LABEL: HBNG_cmeqz_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0 excl V0[0:15] per 4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; cmtst v0.8b, v1.8b, v0.8b
; CHECK-LABEL: HBNG_cmtst_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- 0 excl V1[0:7] | V0[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; fcmgt v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_fcmgt_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- 0 excl V0[0:15] | V1[0:15] per 4\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <16 x i8> @and16(<16 x i8> %a, <16 x i8> %b) {
entry:
  %and.i = and <16 x i8> %b, %a
  ret <16 x i8> %and.i
}

define <8 x i8> @eor8(<8 x i8> %a, <8 x i8> %b) {
entry:
  %xor.i = xor <8 x i8> %b, %a
  ret <8 x i8> %xor.i
}

define <16 x i8> @bic16(<16 x i8> %a, <16 x i8> %b) {
entry:
  %not.i = xor <16 x i8> %b, splat (i8 -1)
  %and.i = and <16 x i8> %a, %not.i
  ret <16 x i8> %and.i
}

define <8 x i8> @not8(<8 x i8> %a) {
entry:
  %not.i = xor <8 x i8> %a, splat (i8 -1)
  ret <8 x i8> %not.i
}

define <4 x i32> @bicimm(<4 x i32> %a) {
entry:
  %and.i = and <4 x i32> %a, splat (i32 -65281)
  ret <4 x i32> %and.i
}

define <4 x i16> @orrimm(<4 x i16> %a) {
entry:
  %or.i = or <4 x i16> %a, splat (i16 3)
  ret <4 x i16> %or.i
}

define <4 x i32> @cmhs(<4 x i32> %a, <4 x i32> %b) {
entry:
  %cmp.i = icmp uge <4 x i32> %a, %b
  %sext.i = sext <4 x i1> %cmp.i to <4 x i32>
  ret <4 x i32> %sext.i
}

define <4 x i32> @cmeqz(<4 x i32> %a) {
entry:
  %0 = icmp eq <4 x i32> %a, zeroinitializer
  %vceqz.i = sext <4 x i1> %0 to <4 x i32>
  ret <4 x i32> %vceqz.i
}

define <8 x i8> @cmtst(<8 x i8> %a, <8 x i8> %b) {
entry:
  %0 = and <8 x i8> %b, %a
  %1 = icmp ne <8 x i8> %0, zeroinitializer
  %vtst.i = sext <8 x i1> %1 to <8 x i8>
  ret <8 x i8> %vtst.i
}

define <4 x i32> @fcmgt(<4 x float> %a, <4 x float> %b) {
entry:
  %cmp.i = fcmp ogt <4 x float> %a, %b
  %sext.i = sext <4 x i1> %cmp.i to <4 x i32>
  ret <4 x i32> %sext.i
}
