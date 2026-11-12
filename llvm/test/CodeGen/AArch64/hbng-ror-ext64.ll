; Register rotates (ror w, ror x: the value and the amount) and the 64-bit
; extended-register compare against sp of a probed dynamic alloca
; (cmp sp, x0: flags only, nothing written).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; ror w0, w0, w1
; CHECK-LABEL: HBNG_rotr_w_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; neg w8, w1 ; ror x0, x0, x8
; CHECK-LABEL: HBNG_rotl_x_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X8 <- X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X8\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; stp x29, x30, [sp, #-16]! ; mov x29, sp ; add x9, x0, #15 ; mov x8, sp ; and x9, x9, #0xfffffffffffffff0 ; sub x0, x8, x9
; CHECK-LABEL: HBNG_dyn_alloca_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "M[<SP> - 16 .. +8] <- FP\n"
; CHECK-NEXT:  .ascii "M[<SP> - 8 .. +8] <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 16\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "FP <- SP\n"
; CHECK-NEXT:  .ascii "<FP> <- <SP>\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X9 <- X0\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X8 <- SP\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "X0 <- X8 | X9\n"
; CHECK-NEXT:  .ascii "END\n"
; sub sp, sp, #1, lsl #12 ; cmp sp, x0 ; b.le .LBB2_3
; CHECK-LABEL: HBNG_dyn_alloca_n_1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 4096\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; ldr xzr, [sp] ; b .LBB2_1
; CHECK-LABEL: HBNG_dyn_alloca_n_2_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; str x0, [x18] ; mov sp, x0 ; ldr xzr, [sp] ; bl use
; CHECK-LABEL: HBNG_dyn_alloca_n_3_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "SP <- X0\n"
; CHECK-NEXT:  .ascii "<SP> <- <X0>\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "LR <- 0\n"
; CHECK-NEXT:  .ascii "END\n"
; mov sp, x29 ; ldp x29, x30, [sp], #16 ; str x29, [x18]
; CHECK-LABEL: HBNG_dyn_alloca_n_3_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "SP <- FP\n"
; CHECK-NEXT:  .ascii "<SP> <- <FP>\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "FP <- M[<SP> .. +8]\n"
; CHECK-NEXT:  .ascii "LR <- M[<SP> + 8 .. +8]\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + 16\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "send X29\n"
; CHECK-NEXT:  .ascii "<FP> <- <X29>\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "END\n"

define i32 @rotr_w(i32 %x, i32 %n) {
  %r = call i32 @llvm.fshr.i32(i32 %x, i32 %x, i32 %n)
  ret i32 %r
}

define i64 @rotl_x(i64 %x, i64 %n) {
  %r = call i64 @llvm.fshl.i64(i64 %x, i64 %x, i64 %n)
  ret i64 %r
}

define void @dyn_alloca(i64 %n) "probe-stack"="inline-asm" {
  %p = alloca i8, i64 %n, align 16
  call void @use(ptr %p)
  ret void
}

declare void @use(ptr)

declare i32 @llvm.fshr.i32(i32, i32, i32)

declare i64 @llvm.fshl.i64(i64, i64, i64)
