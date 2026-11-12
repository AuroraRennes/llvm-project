; Pointer authentication in the hint space (a NOP on the Cortex-A53): the
; pointer from the pointer and the modifier (paciasp, autibsp: LR from LR and
; SP; autia1716: X17 from X17 and X16), and nothing for xpaclri (LR keeps its
; tag), pacm and the b-key CFI pseudo (no instruction, no group).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; hint #25 ; str x30, [sp, #-16]! ; bl g
; CHECK-LABEL: HBNG_sign_a_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "M[<SP> - 16 .. +8] <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 16\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "LR <- 0\n"
; CHECK-NEXT:  .ascii "END\n"
; ldr x30, [sp], #16 ; hint #29
; CHECK-LABEL: HBNG_sign_a_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "LR <- M[<SP> .. +8]\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + 16\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; hint #27 ; str x30, [sp, #-16]! ; bl g
; CHECK-LABEL: HBNG_sign_b_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "M[<SP> - 16 .. +8] <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 16\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "LR <- 0\n"
; CHECK-NEXT:  .ascii "END\n"
; ldr x30, [sp], #16 ; hint #31
; CHECK-LABEL: HBNG_sign_b_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "LR <- M[<SP> .. +8]\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + 16\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; hint #25 ; str x30, [sp, #-16]! ; hint #7 ; mov x0, x30 ; ldr x30, [sp], #16 ; hint #29
; CHECK-LABEL: HBNG_strip_ra_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "M[<SP> - 16 .. +8] <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 16\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- LR\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "LR <- M[<SP> .. +8]\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + 16\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@24\n"
; CHECK-NEXT:  .ascii "END\n"
; hint #39 ; hint #25 ; mov w8, #9 ; str x8, [sp, #-16]! ; mov w0, #1 ; mov w1, #2 ; mov w2, #3 ; mov w3, #4 ; mov w4, #5 ; mov w5, #6 ; mov w6, #7 ; mov w7, #8 ; add x16, sp, #16 ; mov x17, x30 ; adrp x15, .Ltmp1 ; add x15, x15, :lo12:.Ltmp1 ; hint #39 ; hint #12 ; mov x30, x17 ; b h
; CHECK-LABEL: HBNG_tail_fpdiff_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "LR <- LR | SP\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X8 <- 0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "M[<SP> - 16 .. +8] <- X8\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 16\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "X0 <- 0\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "X1 <- 0\n"
; CHECK-NEXT:  .ascii "@24\n"
; CHECK-NEXT:  .ascii "X2 <- 0\n"
; CHECK-NEXT:  .ascii "@28\n"
; CHECK-NEXT:  .ascii "X3 <- 0\n"
; CHECK-NEXT:  .ascii "@32\n"
; CHECK-NEXT:  .ascii "X4 <- 0\n"
; CHECK-NEXT:  .ascii "@36\n"
; CHECK-NEXT:  .ascii "X5 <- 0\n"
; CHECK-NEXT:  .ascii "@40\n"
; CHECK-NEXT:  .ascii "X6 <- 0\n"
; CHECK-NEXT:  .ascii "@44\n"
; CHECK-NEXT:  .ascii "X7 <- 0\n"
; CHECK-NEXT:  .ascii "@48\n"
; CHECK-NEXT:  .ascii "X16 <- SP\n"
; CHECK-NEXT:  .ascii "@52\n"
; CHECK-NEXT:  .ascii "X17 <- LR\n"
; CHECK-NEXT:  .ascii "@56\n"
; CHECK-NEXT:  .ascii "X15 <- 0\n"
; CHECK-NEXT:  .ascii "@60\n"
; CHECK-NEXT:  .ascii "@64\n"
; CHECK-NEXT:  .ascii "@68\n"
; CHECK-NEXT:  .ascii "X17 <- X17 | X16\n"
; CHECK-NEXT:  .ascii "@72\n"
; CHECK-NEXT:  .ascii "LR <- X17\n"
; CHECK-NEXT:  .ascii "@76\n"
; CHECK-NEXT:  .ascii "END\n"

define void @sign_a() "sign-return-address"="all" {
  call void @g()
  ret void
}

define void @sign_b() "sign-return-address"="all" "sign-return-address-key"="b_key" uwtable {
  call void @g()
  ret void
}

define ptr @strip_ra() "sign-return-address"="all" {
  %r = call ptr @llvm.returnaddress(i32 0)
  ret ptr %r
}

define swifttailcc void @tail_fpdiff() "sign-return-address"="all" "branch-protection-pauth-lr" {
  musttail call swifttailcc void @h(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7, i64 8, i64 9)
  ret void
}

declare void @g()

declare swifttailcc void @h(i64, i64, i64, i64, i64, i64, i64, i64, i64)

declare ptr @llvm.returnaddress(i32)
