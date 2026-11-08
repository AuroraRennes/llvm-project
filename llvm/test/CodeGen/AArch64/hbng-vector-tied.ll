; Vector instructions that read their destination: INS (also written MOV)
; keeps the other lanes of Vd and adds the inserted value; BSL/BIT/BIF read
; Vd, Vn and Vm.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; CHECK-LABEL: HBNG_ins_gpr32_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit W0\n"
; CHECK-LABEL: HBNG_ins_gpr8_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit W0\n"
; CHECK-LABEL: HBNG_ins_lane_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit Q1\n"
; CHECK-LABEL: HBNG_bsl_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit Q1 bit Q2\n"
; CHECK-LABEL: HBNG_bit_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit Q1 bit Q2\n"
; CHECK-LABEL: HBNG_bif_annot:
; CHECK-NEXT:  .ascii "Q0 <- Q0 bit Q1 bit Q2\n"

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local noundef <4 x i32> @ins_gpr32(<4 x i32> noundef %v, i32 noundef %x) local_unnamed_addr {
entry:
  %vset_lane = insertelement <4 x i32> %v, i32 %x, i64 2
  ret <4 x i32> %vset_lane
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local noundef <16 x i8> @ins_gpr8(<16 x i8> noundef %v, i8 noundef %x) local_unnamed_addr {
entry:
  %vset_lane = insertelement <16 x i8> %v, i8 %x, i64 5
  ret <16 x i8> %vset_lane
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local noundef <4 x i32> @ins_lane(<4 x i32> noundef %a, <4 x i32> noundef %b) local_unnamed_addr {
entry:
  %vset_lane = shufflevector <4 x i32> %a, <4 x i32> %b, <4 x i32> <i32 0, i32 7, i32 2, i32 3>
  ret <4 x i32> %vset_lane
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local <16 x i8> @bsl(<16 x i8> noundef %m, <16 x i8> noundef %a, <16 x i8> noundef %b) local_unnamed_addr {
entry:
  %vbsl.i = and <16 x i8> %a, %m
  %0 = xor <16 x i8> %m, splat (i8 -1)
  %vbsl1.i = and <16 x i8> %b, %0
  %vbsl2.i = or disjoint <16 x i8> %vbsl1.i, %vbsl.i
  ret <16 x i8> %vbsl2.i
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local <16 x i8> @bit(<16 x i8> noundef %d, <16 x i8> noundef %a, <16 x i8> noundef %m) local_unnamed_addr {
entry:
  %vbsl.i = and <16 x i8> %m, %a
  %0 = xor <16 x i8> %m, splat (i8 -1)
  %vbsl1.i = and <16 x i8> %d, %0
  %vbsl2.i = or disjoint <16 x i8> %vbsl.i, %vbsl1.i
  ret <16 x i8> %vbsl2.i
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local <16 x i8> @bif(<16 x i8> noundef %d, <16 x i8> noundef %a, <16 x i8> noundef %m) local_unnamed_addr {
entry:
  %vbsl.i = and <16 x i8> %m, %d
  %0 = xor <16 x i8> %m, splat (i8 -1)
  %vbsl1.i = and <16 x i8> %a, %0
  %vbsl2.i = or disjoint <16 x i8> %vbsl.i, %vbsl1.i
  ret <16 x i8> %vbsl2.i
}
