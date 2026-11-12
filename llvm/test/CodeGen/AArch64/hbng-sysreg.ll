; System registers and barriers: an mrs gives an untagged value (the thread
; pointer, FPCR, counters are not data the program read or received), an msr
; writes nothing (NZCV saved across a call and restored: the flags carry no
; tag; msr daifset/daifclr: interrupts masked or unmasked), and a barrier
; moves no data.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; CHECK-LABEL: HBNG_tp_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_fence_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; mrs x0, FPCR
; CHECK-LABEL: HBNG_get_fpcr_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- 0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; msr FPCR, x0
; CHECK-LABEL: HBNG_set_fpcr_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; str x30, [sp, #-32]! ; stp x20, x19, [sp, #16] ; cmp w0, w1 ; mov w19, w5 ; ccmp w2, w3, #4, gt ; mrs x20, NZCV ; csel w0, w5, w4, ne ; bl callee
; CHECK-LABEL: HBNG_nzcv_across_call_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "M[<SP> - 32 .. +8] <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> - 32\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "M[<SP> + 16 .. +8] <- X20\n"
; CHECK-NEXT:  .ascii "M[<SP> + 24 .. +8] <- X19\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X19 <- X5\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "X20 <- 0\n"
; CHECK-NEXT:  .ascii "@24\n"
; CHECK-NEXT:  .ascii "X0 <- X5 | X4\n"
; CHECK-NEXT:  .ascii "@28\n"
; CHECK-NEXT:  .ascii "LR <- 0\n"
; CHECK-NEXT:  .ascii "END\n"
; msr NZCV, x20 ; csel w0, w0, w19, ne ; bl callee
; CHECK-LABEL: HBNG_nzcv_across_call_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X19\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "LR <- 0\n"
; CHECK-NEXT:  .ascii "END\n"
; ldp x20, x19, [sp, #16] ; ldr x30, [sp], #32
; CHECK-LABEL: HBNG_nzcv_across_call_split_1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X20 <- M[<SP> + 16 .. +8]\n"
; CHECK-NEXT:  .ascii "X19 <- M[<SP> + 24 .. +8]\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "LR <- M[<SP> .. +8]\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + 32\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "END\n"
; msr DAIFSet, #2
; CHECK-LABEL: HBNG_mask_irq_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; msr DAIFClr, #2
; CHECK-LABEL: HBNG_unmask_irq_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define ptr @tp() {
  %p = call ptr @llvm.thread.pointer.p0()
  ret ptr %p
}

define void @fence() {
  fence seq_cst
  ret void
}

define i64 @get_fpcr() {
  %r = call i64 @llvm.aarch64.get.fpcr()
  ret i64 %r
}

define void @set_fpcr(i64 %v) {
  call void @llvm.aarch64.set.fpcr(i64 %v)
  ret void
}

define i32 @nzcv_across_call(i32 %s0, i32 %s1, i32 %s2, i32 %s3, i32 %x, i32 %y) {
  %c0 = icmp sgt i32 %s0, %s1
  %c1 = icmp ne i32 %s2, %s3
  %a = and i1 %c0, %c1
  %s = select i1 %a, i32 %y, i32 %x
  %o = call i32 @callee(i32 %s)
  %z1 = select i1 %a, i32 %o, i32 %y
  %p = call i32 @callee(i32 %z1)
  ret i32 %p
}

define void @mask_irq() {
  call void @llvm.write_register.i64(metadata !0, i64 2)
  ret void
}

define void @unmask_irq() {
  call void @llvm.write_register.i64(metadata !1, i64 2)
  ret void
}

declare ptr @llvm.thread.pointer.p0()

declare i32 @callee(i32)

declare i64 @llvm.aarch64.get.fpcr()

declare void @llvm.aarch64.set.fpcr(i64)

declare void @llvm.write_register.i64(metadata, i64)

!0 = !{!"daifset"}
!1 = !{!"daifclr"}
