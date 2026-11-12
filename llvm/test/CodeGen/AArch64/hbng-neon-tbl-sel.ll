; Table lookups: every byte written gets the union of all table bytes, the
; index listed under "excl" (no pointer tainting, as a scalar lookup in a
; table). tbx keeps the old byte for an index out of range: byte k also from
; the old byte k, one line per byte. Bitwise selects:
; byte k from byte k of the two data operands ("per 1"), the mask under
; "excl" (bsl selects with its destination, bit/bif with their third operand).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; tbl v0.16b, { v0.16b }, v1.16b
; CHECK-LABEL: HBNG_tbl1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] excl V1[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; tbl v0.8b, { v0.16b, v1.16b }, v2.8b
; CHECK-LABEL: HBNG_tbl2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:7] <- V0[0:15] | V1[0:15] excl V2[0:7] per 1\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; tbl v0.16b, { v0.16b, v1.16b, v2.16b, v3.16b }, v4.16b
; CHECK-LABEL: HBNG_tbl4_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V0[0:15] | V1[0:15] | V2[0:15] | V3[0:15] excl V4[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; tbx v0.16b, { v1.16b }, v2.16b
; CHECK-LABEL: HBNG_tbx_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0] <- V0[0] | V1[0:15] excl V2[0]\n"
; CHECK-NEXT:  .ascii "V0[1] <- V0[1] | V1[0:15] excl V2[1]\n"
; CHECK-NEXT:  .ascii "V0[2] <- V0[2] | V1[0:15] excl V2[2]\n"
; CHECK-NEXT:  .ascii "V0[3] <- V0[3] | V1[0:15] excl V2[3]\n"
; CHECK-NEXT:  .ascii "V0[4] <- V0[4] | V1[0:15] excl V2[4]\n"
; CHECK-NEXT:  .ascii "V0[5] <- V0[5] | V1[0:15] excl V2[5]\n"
; CHECK-NEXT:  .ascii "V0[6] <- V0[6] | V1[0:15] excl V2[6]\n"
; CHECK-NEXT:  .ascii "V0[7] <- V0[7] | V1[0:15] excl V2[7]\n"
; CHECK-NEXT:  .ascii "V0[8] <- V0[8] | V1[0:15] excl V2[8]\n"
; CHECK-NEXT:  .ascii "V0[9] <- V0[9] | V1[0:15] excl V2[9]\n"
; CHECK-NEXT:  .ascii "V0[10] <- V0[10] | V1[0:15] excl V2[10]\n"
; CHECK-NEXT:  .ascii "V0[11] <- V0[11] | V1[0:15] excl V2[11]\n"
; CHECK-NEXT:  .ascii "V0[12] <- V0[12] | V1[0:15] excl V2[12]\n"
; CHECK-NEXT:  .ascii "V0[13] <- V0[13] | V1[0:15] excl V2[13]\n"
; CHECK-NEXT:  .ascii "V0[14] <- V0[14] | V1[0:15] excl V2[14]\n"
; CHECK-NEXT:  .ascii "V0[15] <- V0[15] | V1[0:15] excl V2[15]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; tbx v0.8b, { v1.16b, v2.16b }, v3.8b
; CHECK-LABEL: HBNG_tbx2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0] <- V0[0] | V1[0:15] | V2[0:15] excl V3[0]\n"
; CHECK-NEXT:  .ascii "V0[1] <- V0[1] | V1[0:15] | V2[0:15] excl V3[1]\n"
; CHECK-NEXT:  .ascii "V0[2] <- V0[2] | V1[0:15] | V2[0:15] excl V3[2]\n"
; CHECK-NEXT:  .ascii "V0[3] <- V0[3] | V1[0:15] | V2[0:15] excl V3[3]\n"
; CHECK-NEXT:  .ascii "V0[4] <- V0[4] | V1[0:15] | V2[0:15] excl V3[4]\n"
; CHECK-NEXT:  .ascii "V0[5] <- V0[5] | V1[0:15] | V2[0:15] excl V3[5]\n"
; CHECK-NEXT:  .ascii "V0[6] <- V0[6] | V1[0:15] | V2[0:15] excl V3[6]\n"
; CHECK-NEXT:  .ascii "V0[7] <- V0[7] | V1[0:15] | V2[0:15] excl V3[7]\n"
; CHECK-NEXT:  .ascii "V0[8:15] <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; bsl v0.16b, v1.16b, v2.16b
; CHECK-LABEL: HBNG_bsl_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "V0[0:15] <- V1[0:15] | V2[0:15] per 1 excl V0[0:15] per 1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define <16 x i8> @tbl1(<16 x i8> %t, <16 x i8> %i) {
entry:
  %vtbl1.i = tail call <16 x i8> @llvm.aarch64.neon.tbl1.v16i8(<16 x i8> %t, <16 x i8> %i)
  ret <16 x i8> %vtbl1.i
}

define <8 x i8> @tbl2([2 x <16 x i8>] alignstack(16) %t.coerce, <8 x i8> %i) {
entry:
  %__p0.coerce.fca.0.extract.i = extractvalue [2 x <16 x i8>] %t.coerce, 0
  %__p0.coerce.fca.1.extract.i = extractvalue [2 x <16 x i8>] %t.coerce, 1
  %vtbl2.i = tail call <8 x i8> @llvm.aarch64.neon.tbl2.v8i8(<16 x i8> %__p0.coerce.fca.0.extract.i, <16 x i8> %__p0.coerce.fca.1.extract.i, <8 x i8> %i)
  ret <8 x i8> %vtbl2.i
}

define <16 x i8> @tbl4([4 x <16 x i8>] alignstack(16) %t.coerce, <16 x i8> %i) {
entry:
  %__p0.coerce.fca.0.extract.i = extractvalue [4 x <16 x i8>] %t.coerce, 0
  %__p0.coerce.fca.1.extract.i = extractvalue [4 x <16 x i8>] %t.coerce, 1
  %__p0.coerce.fca.2.extract.i = extractvalue [4 x <16 x i8>] %t.coerce, 2
  %__p0.coerce.fca.3.extract.i = extractvalue [4 x <16 x i8>] %t.coerce, 3
  %vtbl4.i = tail call <16 x i8> @llvm.aarch64.neon.tbl4.v16i8(<16 x i8> %__p0.coerce.fca.0.extract.i, <16 x i8> %__p0.coerce.fca.1.extract.i, <16 x i8> %__p0.coerce.fca.2.extract.i, <16 x i8> %__p0.coerce.fca.3.extract.i, <16 x i8> %i)
  ret <16 x i8> %vtbl4.i
}

define <16 x i8> @tbx(<16 x i8> %d, <16 x i8> %t, <16 x i8> %i) {
entry:
  %vtbx1.i = tail call <16 x i8> @llvm.aarch64.neon.tbx1.v16i8(<16 x i8> %d, <16 x i8> %t, <16 x i8> %i)
  ret <16 x i8> %vtbx1.i
}

define <8 x i8> @tbx2(<8 x i8> %d, <16 x i8> %t0, <16 x i8> %t1, <8 x i8> %i) {
entry:
  %r = tail call <8 x i8> @llvm.aarch64.neon.tbx2.v8i8(<8 x i8> %d, <16 x i8> %t0, <16 x i8> %t1, <8 x i8> %i)
  ret <8 x i8> %r
}

define <16 x i8> @bsl(<16 x i8> %m, <16 x i8> %a, <16 x i8> %b) {
entry:
  %vbsl.i = and <16 x i8> %a, %m
  %0 = xor <16 x i8> %m, splat (i8 -1)
  %vbsl1.i = and <16 x i8> %b, %0
  %vbsl2.i = or disjoint <16 x i8> %vbsl1.i, %vbsl.i
  ret <16 x i8> %vbsl2.i
}

declare <16 x i8> @llvm.aarch64.neon.tbl1.v16i8(<16 x i8>, <16 x i8>)

declare <8 x i8> @llvm.aarch64.neon.tbl2.v8i8(<16 x i8>, <16 x i8>, <8 x i8>)

declare <16 x i8> @llvm.aarch64.neon.tbl4.v16i8(<16 x i8>, <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8>)

declare <16 x i8> @llvm.aarch64.neon.tbx1.v16i8(<16 x i8>, <16 x i8>, <16 x i8>)

declare <8 x i8> @llvm.aarch64.neon.tbx2.v8i8(<8 x i8>, <16 x i8>, <16 x i8>, <8 x i8>)
