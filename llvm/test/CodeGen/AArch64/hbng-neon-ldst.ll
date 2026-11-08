; NEON structured loads and stores (ld1-ld4, st1-st4), one line per register
; of the list, each with its send of the base:
;  - ld1/st1 of several registers: register r at byte 16r (8r for D);
;  - ld2-ld4/st2-st4: "+ k step s xN", N elements, the i-th at byte k + i*s;
;  - ld1r-ld4r: "rep N", one element replicated N times;
;  - one lane: V<n>[lo:hi] for the lane, the other bytes kept.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s --check-prefix=ASM

; The sends: the base once per register of the list, before the access.
; ASM-LABEL: ld1q:
; ASM:         str x0, [x18]
; ASM-NEXT:    ldr q0, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld1q2:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    ld1 { v0.16b, v1.16b }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld1d4:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    stp x0, x0, [x18]
; ASM-NEXT:    ld1 { v0.4h, v1.4h, v2.4h, v3.4h }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld2s:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    ld2 { v0.4s, v1.4s }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld3d:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    str x0, [x18]
; ASM-NEXT:    ld3 { v0.4h, v1.4h, v2.4h }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld4:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    stp x0, x0, [x18]
; ASM-NEXT:    ld4 { v0.16b, v1.16b, v2.16b, v3.16b }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld1r:
; ASM:         str x0, [x18]
; ASM-NEXT:    ld1r { v0.4s }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld2r:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    ld2r { v0.4h, v1.4h }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld1lane8:
; ASM:         str x0, [x18]
; ASM-NEXT:    ld1 { v0.b }[5], [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld1lane32:
; ASM:         str x0, [x18]
; ASM-NEXT:    ld1 { v0.s }[2], [x0]
; ASM-NEXT:    ret
; ASM-LABEL: ld2lane:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    ld2 { v0.h, v1.h }[3], [x0]
; ASM-NEXT:    ret
; ASM-LABEL: st1lane:
; ASM:         str x0, [x18]
; ASM-NEXT:    st1 { v0.s }[2], [x0]
; ASM-NEXT:    ret
; ASM-LABEL: st1x3:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    str x0, [x18]
; ASM-NEXT:    st1 { v0.8b, v1.8b, v2.8b }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: st2d:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    st2 { v0.2s, v1.2s }, [x0]
; ASM-NEXT:    ret
; ASM-LABEL: st4:
; ASM:         stp x0, x0, [x18]
; ASM-NEXT:    stp x0, x0, [x18]
; ASM-NEXT:    st4 { v0.16b, v1.16b, v2.16b, v3.16b }, [x0]
; ASM-NEXT:    ret

; CHECK-LABEL: HBNG_ld1q_annot:
; CHECK-NEXT:  .ascii "Q0 <- [<X0> + 16*Imm(0)]_128\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld1q2_annot:
; CHECK-NEXT:  .ascii "Q0 <- [<X0> + Imm(0)]_128\n"
; CHECK-NEXT:  .ascii "Q1 <- [<X0> + Imm(0) + 16]_128\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld1d4_annot:
; CHECK-NEXT:  .ascii "D0 <- [<X0> + Imm(0)]_64\n"
; CHECK-NEXT:  .ascii "D1 <- [<X0> + Imm(0) + 8]_64\n"
; CHECK-NEXT:  .ascii "D2 <- [<X0> + Imm(0) + 16]_64\n"
; CHECK-NEXT:  .ascii "D3 <- [<X0> + Imm(0) + 24]_64\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld2s_annot:
; CHECK-NEXT:  .ascii "Q0 <- [<X0> + Imm(0) step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "Q1 <- [<X0> + Imm(0) + 4 step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld3d_annot:
; CHECK-NEXT:  .ascii "D0 <- [<X0> + Imm(0) step 6 x4]_16\n"
; CHECK-NEXT:  .ascii "D1 <- [<X0> + Imm(0) + 2 step 6 x4]_16\n"
; CHECK-NEXT:  .ascii "D2 <- [<X0> + Imm(0) + 4 step 6 x4]_16\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld4_annot:
; CHECK-NEXT:  .ascii "Q0 <- [<X0> + Imm(0) step 4 x16]_8\n"
; CHECK-NEXT:  .ascii "Q1 <- [<X0> + Imm(0) + 1 step 4 x16]_8\n"
; CHECK-NEXT:  .ascii "Q2 <- [<X0> + Imm(0) + 2 step 4 x16]_8\n"
; CHECK-NEXT:  .ascii "Q3 <- [<X0> + Imm(0) + 3 step 4 x16]_8\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld1r_annot:
; CHECK-NEXT:  .ascii "Q0 <- [<X0> + Imm(0) rep 4]_32\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld2r_annot:
; CHECK-NEXT:  .ascii "D0 <- [<X0> + Imm(0) rep 4]_16\n"
; CHECK-NEXT:  .ascii "D1 <- [<X0> + Imm(0) + 2 rep 4]_16\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld1lane8_annot:
; CHECK-NEXT:  .ascii "V0[5:5] <- [<X0> + Imm(0)]_8\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld1lane32_annot:
; CHECK-NEXT:  .ascii "V0[8:11] <- [<X0> + Imm(0)]_32\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_ld2lane_annot:
; CHECK-NEXT:  .ascii "V0[6:7] <- [<X0> + Imm(0)]_16\n"
; CHECK-NEXT:  .ascii "V1[6:7] <- [<X0> + Imm(0) + 2]_16\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_st1lane_annot:
; CHECK-NEXT:  .ascii "[<X0> + Imm(0)]_32 <- V0[8:11]\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_st1x3_annot:
; CHECK-NEXT:  .ascii "[<X0> + Imm(0)]_64 <- D0\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 8]_64 <- D1\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 16]_64 <- D2\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_st2d_annot:
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) step 8 x2]_32 <- D0\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 4 step 8 x2]_32 <- D1\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_st4_annot:
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) step 4 x16]_8 <- Q0\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 1 step 4 x16]_8 <- Q1\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 2 step 4 x16]_8 <- Q2\n"
; CHECK-NEXT:  .ascii "[<X0> + Imm(0) + 3 step 4 x16]_8 <- Q3\n"
; CHECK-NEXT:  .ascii "END\n"

; Post-index: the address is the base; the write-back of a base that is not
; SP or FP needs no line (the base is sent again at its next access).
; ASM-LABEL: ld2_post_imm:
; ASM:         stp x19, x19, [x18]
; ASM-NEXT:    ld2 { v0.4s, v1.4s }, [x19], #32
; ASM-LABEL: ld2_post_reg:
; ASM:         stp x19, x19, [x18]
; ASM-NEXT:    ld2 { v0.4s, v1.4s }, [x19], x1
; CHECK-LABEL: HBNG_ld2_post_imm_annot:
; CHECK:       .ascii "Q0 <- [<X19> + Imm(0) step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "Q1 <- [<X19> + Imm(0) + 4 step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "LR <- Imm(0)\n"
; CHECK-LABEL: HBNG_ld2_post_reg_annot:
; CHECK:       .ascii "Q0 <- [<X19> + Imm(0) step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "Q1 <- [<X19> + Imm(0) + 4 step 8 x4]_32\n"
; CHECK-NEXT:  .ascii "LR <- Imm(0)\n"

%struct.uint8x16x2_t = type { [2 x <16 x i8>] }
%struct.uint16x4x4_t = type { [4 x <4 x i16>] }
%struct.uint32x4x2_t = type { [2 x <4 x i32>] }
%struct.uint16x4x3_t = type { [3 x <4 x i16>] }
%struct.uint8x16x4_t = type { [4 x <16 x i8>] }
%struct.uint16x4x2_t = type { [2 x <4 x i16>] }
%struct.uint16x8x2_t = type { [2 x <8 x i16>] }

define <16 x i8> @ld1q(ptr %p) {
entry:
  %0 = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %0
}

define %struct.uint8x16x2_t @ld1q2(ptr %p) {
entry:
  %vld1xN = tail call { <16 x i8>, <16 x i8> } @llvm.aarch64.neon.ld1x2.v16i8.p0(ptr %p)
  %vld1xN.fca.0.extract = extractvalue { <16 x i8>, <16 x i8> } %vld1xN, 0
  %vld1xN.fca.1.extract = extractvalue { <16 x i8>, <16 x i8> } %vld1xN, 1
  %.fca.0.0.insert = insertvalue %struct.uint8x16x2_t poison, <16 x i8> %vld1xN.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint8x16x2_t %.fca.0.0.insert, <16 x i8> %vld1xN.fca.1.extract, 0, 1
  ret %struct.uint8x16x2_t %.fca.0.1.insert
}

declare { <16 x i8>, <16 x i8> } @llvm.aarch64.neon.ld1x2.v16i8.p0(ptr)

define %struct.uint16x4x4_t @ld1d4(ptr %p) {
entry:
  %vld1xN = tail call { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld1x4.v4i16.p0(ptr %p)
  %vld1xN.fca.0.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } %vld1xN, 0
  %vld1xN.fca.1.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } %vld1xN, 1
  %vld1xN.fca.2.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } %vld1xN, 2
  %vld1xN.fca.3.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } %vld1xN, 3
  %.fca.0.0.insert = insertvalue %struct.uint16x4x4_t poison, <4 x i16> %vld1xN.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint16x4x4_t %.fca.0.0.insert, <4 x i16> %vld1xN.fca.1.extract, 0, 1
  %.fca.0.2.insert = insertvalue %struct.uint16x4x4_t %.fca.0.1.insert, <4 x i16> %vld1xN.fca.2.extract, 0, 2
  %.fca.0.3.insert = insertvalue %struct.uint16x4x4_t %.fca.0.2.insert, <4 x i16> %vld1xN.fca.3.extract, 0, 3
  ret %struct.uint16x4x4_t %.fca.0.3.insert
}

declare { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld1x4.v4i16.p0(ptr)

define %struct.uint32x4x2_t @ld2s(ptr %p) {
entry:
  %vld2 = tail call { <4 x i32>, <4 x i32> } @llvm.aarch64.neon.ld2.v4i32.p0(ptr %p)
  %vld2.fca.0.extract = extractvalue { <4 x i32>, <4 x i32> } %vld2, 0
  %vld2.fca.1.extract = extractvalue { <4 x i32>, <4 x i32> } %vld2, 1
  %.fca.0.0.insert = insertvalue %struct.uint32x4x2_t poison, <4 x i32> %vld2.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint32x4x2_t %.fca.0.0.insert, <4 x i32> %vld2.fca.1.extract, 0, 1
  ret %struct.uint32x4x2_t %.fca.0.1.insert
}

declare { <4 x i32>, <4 x i32> } @llvm.aarch64.neon.ld2.v4i32.p0(ptr)

define %struct.uint16x4x3_t @ld3d(ptr %p) {
entry:
  %vld3 = tail call { <4 x i16>, <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld3.v4i16.p0(ptr %p)
  %vld3.fca.0.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16> } %vld3, 0
  %vld3.fca.1.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16> } %vld3, 1
  %vld3.fca.2.extract = extractvalue { <4 x i16>, <4 x i16>, <4 x i16> } %vld3, 2
  %.fca.0.0.insert = insertvalue %struct.uint16x4x3_t poison, <4 x i16> %vld3.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint16x4x3_t %.fca.0.0.insert, <4 x i16> %vld3.fca.1.extract, 0, 1
  %.fca.0.2.insert = insertvalue %struct.uint16x4x3_t %.fca.0.1.insert, <4 x i16> %vld3.fca.2.extract, 0, 2
  ret %struct.uint16x4x3_t %.fca.0.2.insert
}

declare { <4 x i16>, <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld3.v4i16.p0(ptr)

define %struct.uint8x16x4_t @ld4(ptr %p) {
entry:
  %vld4 = tail call { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } @llvm.aarch64.neon.ld4.v16i8.p0(ptr %p)
  %vld4.fca.0.extract = extractvalue { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } %vld4, 0
  %vld4.fca.1.extract = extractvalue { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } %vld4, 1
  %vld4.fca.2.extract = extractvalue { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } %vld4, 2
  %vld4.fca.3.extract = extractvalue { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } %vld4, 3
  %.fca.0.0.insert = insertvalue %struct.uint8x16x4_t poison, <16 x i8> %vld4.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint8x16x4_t %.fca.0.0.insert, <16 x i8> %vld4.fca.1.extract, 0, 1
  %.fca.0.2.insert = insertvalue %struct.uint8x16x4_t %.fca.0.1.insert, <16 x i8> %vld4.fca.2.extract, 0, 2
  %.fca.0.3.insert = insertvalue %struct.uint8x16x4_t %.fca.0.2.insert, <16 x i8> %vld4.fca.3.extract, 0, 3
  ret %struct.uint8x16x4_t %.fca.0.3.insert
}

declare { <16 x i8>, <16 x i8>, <16 x i8>, <16 x i8> } @llvm.aarch64.neon.ld4.v16i8.p0(ptr)

define <4 x i32> @ld1r(ptr %p) {
entry:
  %0 = load i32, ptr %p, align 4
  %1 = insertelement <4 x i32> poison, i32 %0, i64 0
  %lane = shufflevector <4 x i32> %1, <4 x i32> poison, <4 x i32> zeroinitializer
  ret <4 x i32> %lane
}

define %struct.uint16x4x2_t @ld2r(ptr %p) {
entry:
  %vld2 = tail call { <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld2r.v4i16.p0(ptr %p)
  %vld2.fca.0.extract = extractvalue { <4 x i16>, <4 x i16> } %vld2, 0
  %vld2.fca.1.extract = extractvalue { <4 x i16>, <4 x i16> } %vld2, 1
  %.fca.0.0.insert = insertvalue %struct.uint16x4x2_t poison, <4 x i16> %vld2.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint16x4x2_t %.fca.0.0.insert, <4 x i16> %vld2.fca.1.extract, 0, 1
  ret %struct.uint16x4x2_t %.fca.0.1.insert
}

declare { <4 x i16>, <4 x i16> } @llvm.aarch64.neon.ld2r.v4i16.p0(ptr)

define <16 x i8> @ld1lane8(ptr %p, <16 x i8> %v) {
entry:
  %0 = load i8, ptr %p, align 1
  %vld1_lane = insertelement <16 x i8> %v, i8 %0, i64 5
  ret <16 x i8> %vld1_lane
}

define <4 x i32> @ld1lane32(ptr %p, <4 x i32> %v) {
entry:
  %0 = load i32, ptr %p, align 4
  %vld1_lane = insertelement <4 x i32> %v, i32 %0, i64 2
  ret <4 x i32> %vld1_lane
}

define %struct.uint16x8x2_t @ld2lane(ptr %p, [2 x <8 x i16>] alignstack(16) %v.coerce) {
entry:
  %v.coerce.fca.0.extract = extractvalue [2 x <8 x i16>] %v.coerce, 0
  %v.coerce.fca.1.extract = extractvalue [2 x <8 x i16>] %v.coerce, 1
  %vld2_lane = tail call { <8 x i16>, <8 x i16> } @llvm.aarch64.neon.ld2lane.v8i16.p0(<8 x i16> %v.coerce.fca.0.extract, <8 x i16> %v.coerce.fca.1.extract, i64 3, ptr %p)
  %vld2_lane.fca.0.extract = extractvalue { <8 x i16>, <8 x i16> } %vld2_lane, 0
  %vld2_lane.fca.1.extract = extractvalue { <8 x i16>, <8 x i16> } %vld2_lane, 1
  %.fca.0.0.insert = insertvalue %struct.uint16x8x2_t poison, <8 x i16> %vld2_lane.fca.0.extract, 0, 0
  %.fca.0.1.insert = insertvalue %struct.uint16x8x2_t %.fca.0.0.insert, <8 x i16> %vld2_lane.fca.1.extract, 0, 1
  ret %struct.uint16x8x2_t %.fca.0.1.insert
}

declare { <8 x i16>, <8 x i16> } @llvm.aarch64.neon.ld2lane.v8i16.p0(<8 x i16>, <8 x i16>, i64, ptr)

define void @st1lane(ptr %p, <4 x i32> %v) {
entry:
  %0 = extractelement <4 x i32> %v, i64 2
  store i32 %0, ptr %p, align 4
  ret void
}

define void @st1x3(ptr %p, [3 x <8 x i8>] alignstack(8) %v.coerce) {
entry:
  %v.coerce.fca.0.extract = extractvalue [3 x <8 x i8>] %v.coerce, 0
  %v.coerce.fca.1.extract = extractvalue [3 x <8 x i8>] %v.coerce, 1
  %v.coerce.fca.2.extract = extractvalue [3 x <8 x i8>] %v.coerce, 2
  tail call void @llvm.aarch64.neon.st1x3.v8i8.p0(<8 x i8> %v.coerce.fca.0.extract, <8 x i8> %v.coerce.fca.1.extract, <8 x i8> %v.coerce.fca.2.extract, ptr %p)
  ret void
}

declare void @llvm.aarch64.neon.st1x3.v8i8.p0(<8 x i8>, <8 x i8>, <8 x i8>, ptr captures(none))

define void @st2d(ptr %p, [2 x <2 x i32>] alignstack(8) %v.coerce) {
entry:
  %v.coerce.fca.0.extract = extractvalue [2 x <2 x i32>] %v.coerce, 0
  %v.coerce.fca.1.extract = extractvalue [2 x <2 x i32>] %v.coerce, 1
  tail call void @llvm.aarch64.neon.st2.v2i32.p0(<2 x i32> %v.coerce.fca.0.extract, <2 x i32> %v.coerce.fca.1.extract, ptr %p)
  ret void
}

declare void @llvm.aarch64.neon.st2.v2i32.p0(<2 x i32>, <2 x i32>, ptr captures(none))

define void @st4(ptr %p, [4 x <16 x i8>] alignstack(16) %v.coerce) {
entry:
  %v.coerce.fca.0.extract = extractvalue [4 x <16 x i8>] %v.coerce, 0
  %v.coerce.fca.1.extract = extractvalue [4 x <16 x i8>] %v.coerce, 1
  %v.coerce.fca.2.extract = extractvalue [4 x <16 x i8>] %v.coerce, 2
  %v.coerce.fca.3.extract = extractvalue [4 x <16 x i8>] %v.coerce, 3
  tail call void @llvm.aarch64.neon.st4.v16i8.p0(<16 x i8> %v.coerce.fca.0.extract, <16 x i8> %v.coerce.fca.1.extract, <16 x i8> %v.coerce.fca.2.extract, <16 x i8> %v.coerce.fca.3.extract, ptr %p)
  ret void
}

declare void @llvm.aarch64.neon.st4.v16i8.p0(<16 x i8>, <16 x i8>, <16 x i8>, <16 x i8>, ptr captures(none))

declare void @use(<4 x i32>, <4 x i32>)

define ptr @ld2_post_imm(ptr %p) {
  %v = call { <4 x i32>, <4 x i32> } @llvm.aarch64.neon.ld2.v4i32.p0(ptr %p)
  %a = extractvalue { <4 x i32>, <4 x i32> } %v, 0
  %b = extractvalue { <4 x i32>, <4 x i32> } %v, 1
  call void @use(<4 x i32> %a, <4 x i32> %b)
  %n = getelementptr i8, ptr %p, i64 32
  ret ptr %n
}

define ptr @ld2_post_reg(ptr %p, i64 %s) {
  %v = call { <4 x i32>, <4 x i32> } @llvm.aarch64.neon.ld2.v4i32.p0(ptr %p)
  %a = extractvalue { <4 x i32>, <4 x i32> } %v, 0
  %b = extractvalue { <4 x i32>, <4 x i32> } %v, 1
  call void @use(<4 x i32> %a, <4 x i32> %b)
  %n = getelementptr i8, ptr %p, i64 %s
  ret ptr %n
}
