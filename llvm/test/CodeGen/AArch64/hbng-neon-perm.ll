; NEON permutes and lane duplication move exact bytes, given as a byte map:
; "perm (b0 b1 ...)" sets byte k of the destination from byte bk of the
; sources taken one after the other (a 64-bit source is 8 bytes). The maps
; below follow the definitions of zip, uzp, trn, ext, rev and dup (element).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; zip1 v0.16b, v0.16b, v1.16b
; CHECK-LABEL: HBNG_zip1_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm Q1 (0 16 1 17 2 18 3 19 4 20 5 21 6 22 7 23)\n"
; CHECK-NEXT:  .ascii "END\n"
; zip2 v0.4h, v0.4h, v1.4h
; CHECK-LABEL: HBNG_zip2h_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 perm D1 (4 5 12 13 6 7 14 15)\n"
; CHECK-NEXT:  .ascii "END\n"
; uzp1 v0.8h, v0.8h, v1.8h
; CHECK-LABEL: HBNG_uzp1_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm Q1 (0 1 4 5 8 9 12 13 16 17 20 21 24 25 28 29)\n"
; CHECK-NEXT:  .ascii "END\n"
; uzp1 v0.16b, v0.16b, v1.16b
; CHECK-LABEL: HBNG_uzp1b_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm Q1 (0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30)\n"
; CHECK-NEXT:  .ascii "END\n"
; zip2 v0.2s, v0.2s, v1.2s
; CHECK-LABEL: HBNG_uzp2s_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 perm D1 (4 5 6 7 12 13 14 15)\n"
; CHECK-NEXT:  .ascii "END\n"
; trn2 v0.4s, v0.4s, v1.4s
; CHECK-LABEL: HBNG_trn2_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm Q1 (4 5 6 7 20 21 22 23 12 13 14 15 28 29 30 31)\n"
; CHECK-NEXT:  .ascii "END\n"
; ext v0.16b, v0.16b, v1.16b, #3
; CHECK-LABEL: HBNG_ext_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm Q1 (3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18)\n"
; CHECK-NEXT:  .ascii "END\n"
; ext v0.8b, v0.8b, v1.8b, #6
; CHECK-LABEL: HBNG_ext8_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 perm D1 (6 7 8 9 10 11 12 13)\n"
; CHECK-NEXT:  .ascii "END\n"
; rev16 v0.16b, v0.16b
; CHECK-LABEL: HBNG_rev16_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm (1 0 3 2 5 4 7 6 9 8 11 10 13 12 15 14)\n"
; CHECK-NEXT:  .ascii "END\n"
; rev32 v0.4h, v0.4h
; CHECK-LABEL: HBNG_rev32h_annot:
; CHECK-NEXT:  .ascii "D0 <- D0 perm (2 3 0 1 6 7 4 5)\n"
; CHECK-NEXT:  .ascii "END\n"
; rev64 v0.8h, v0.8h
; CHECK-LABEL: HBNG_rev64h_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm (6 7 4 5 2 3 0 1 14 15 12 13 10 11 8 9)\n"
; CHECK-NEXT:  .ascii "END\n"
; dup v0.4s, v0.s[1]
; CHECK-LABEL: HBNG_duplane_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 perm (4 5 6 7 4 5 6 7 4 5 6 7 4 5 6 7)\n"
; CHECK-NEXT:  .ascii "END\n"
; dup v0.2s, v0.s[3]
; CHECK-LABEL: HBNG_duplane2_annot:
; CHECK-NEXT:  .ascii "D0 <- Q0 perm (12 13 14 15 12 13 14 15)\n"
; CHECK-NEXT:  .ascii "END\n"

define <16 x i8> @zip1(<16 x i8> %a, <16 x i8> %b) {
entry:
  %shuffle.i = shufflevector <16 x i8> %a, <16 x i8> %b, <16 x i32> <i32 0, i32 16, i32 1, i32 17, i32 2, i32 18, i32 3, i32 19, i32 4, i32 20, i32 5, i32 21, i32 6, i32 22, i32 7, i32 23>
  ret <16 x i8> %shuffle.i
}

define <4 x i16> @zip2h(<4 x i16> %a, <4 x i16> %b) {
entry:
  %shuffle.i = shufflevector <4 x i16> %a, <4 x i16> %b, <4 x i32> <i32 2, i32 6, i32 3, i32 7>
  ret <4 x i16> %shuffle.i
}

define <8 x i16> @uzp1(<8 x i16> %a, <8 x i16> %b) {
entry:
  %shuffle.i = shufflevector <8 x i16> %a, <8 x i16> %b, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  ret <8 x i16> %shuffle.i
}

define <16 x i8> @uzp1b(<16 x i8> %a, <16 x i8> %b) {
entry:
  %shuffle.i = shufflevector <16 x i8> %a, <16 x i8> %b, <16 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14, i32 16, i32 18, i32 20, i32 22, i32 24, i32 26, i32 28, i32 30>
  ret <16 x i8> %shuffle.i
}

define <2 x i32> @uzp2s(<2 x i32> %a, <2 x i32> %b) {
entry:
  %shuffle.i = shufflevector <2 x i32> %a, <2 x i32> %b, <2 x i32> <i32 1, i32 3>
  ret <2 x i32> %shuffle.i
}

define <4 x i32> @trn2(<4 x i32> %a, <4 x i32> %b) {
entry:
  %shuffle.i = shufflevector <4 x i32> %a, <4 x i32> %b, <4 x i32> <i32 1, i32 5, i32 3, i32 7>
  ret <4 x i32> %shuffle.i
}

define <16 x i8> @ext(<16 x i8> %a, <16 x i8> %b) {
entry:
  %vext = shufflevector <16 x i8> %a, <16 x i8> %b, <16 x i32> <i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15, i32 16, i32 17, i32 18>
  ret <16 x i8> %vext
}

define <8 x i8> @ext8(<8 x i8> %a, <8 x i8> %b) {
entry:
  %vext = shufflevector <8 x i8> %a, <8 x i8> %b, <8 x i32> <i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13>
  ret <8 x i8> %vext
}

define <16 x i8> @rev16(<16 x i8> %a) {
entry:
  %shuffle.i = shufflevector <16 x i8> %a, <16 x i8> poison, <16 x i32> <i32 1, i32 0, i32 3, i32 2, i32 5, i32 4, i32 7, i32 6, i32 9, i32 8, i32 11, i32 10, i32 13, i32 12, i32 15, i32 14>
  ret <16 x i8> %shuffle.i
}

define <4 x i16> @rev32h(<4 x i16> %a) {
entry:
  %shuffle.i = shufflevector <4 x i16> %a, <4 x i16> poison, <4 x i32> <i32 1, i32 0, i32 3, i32 2>
  ret <4 x i16> %shuffle.i
}

define <8 x i16> @rev64h(<8 x i16> %a) {
entry:
  %shuffle.i = shufflevector <8 x i16> %a, <8 x i16> poison, <8 x i32> <i32 3, i32 2, i32 1, i32 0, i32 7, i32 6, i32 5, i32 4>
  ret <8 x i16> %shuffle.i
}

define <4 x i32> @duplane(<4 x i32> %a) {
entry:
  %lane = shufflevector <4 x i32> %a, <4 x i32> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  ret <4 x i32> %lane
}

define <2 x i32> @duplane2(<4 x i32> %a) {
entry:
  %lane = shufflevector <4 x i32> %a, <4 x i32> poison, <2 x i32> <i32 3, i32 3>
  ret <2 x i32> %lane
}
