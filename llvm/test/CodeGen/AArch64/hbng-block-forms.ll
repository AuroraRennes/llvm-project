; With -hbng-block-forms, the groups of a block are composed into as few forms
; as possible. A form starts with "@<offset of its first instruction>", then one
; "send Xn" per send, then its statements, which name the sends S0,
; S1, ... instead of the registers; "END" closes the block. A new form
; starts where the aliasing of two addresses is undecided, and an opaque
; instruction (svc) stands alone.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -hbng-block-forms -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s --check-prefix=OFF
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -hbng-block-forms -O2 < %s | FileCheck %s --check-prefix=TBL

@g = global [4 x i64] zeroinitializer, align 8

; Two loads from the same pointer: one form, the register sent by the first
; instruction is S1 in the whole block.
; CHECK-LABEL: HBNG_compose_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X0\n"
; CHECK-NEXT:  .ascii "send X0\n"
; CHECK-NEXT:  .ascii "X0 <- M[S1 .. +16]\n"
; CHECK-NEXT:  .ascii "X8 <- M[S1 .. +8]\n"
; CHECK-NEXT:  .ascii "X9 <- M[S1 + 8 .. +8]\n"
; CHECK-NEXT:  .ascii "END\n"
; OFF-LABEL: HBNG_compose_annot:
; OFF-NEXT:  .ascii "@0\n"
; OFF-NEXT:  .ascii "send X0\n"
; OFF-NEXT:  .ascii "send X0\n"
; OFF-NEXT:  .ascii "@4\n"
define i64 @compose(ptr %p) {
  %a = load i64, ptr %p, align 8
  %q = getelementptr i64, ptr %p, i64 1
  %b = load i64, ptr %q, align 8
  %s = add i64 %a, %b
  ret i64 %s
}

; adrp, then the load from the GOT (:got_lo12:g): its offset is that of the
; instruction at @8.
; CHECK-LABEL: HBNG_lo12_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X8\n"
; CHECK-NEXT:  .ascii "send X8\n"
; CHECK-NEXT:  .ascii "X0 <- M[S1 + 8 .. +8]\n"
; CHECK-NEXT:  .ascii "X8 <- M[LO12@8 + S0 .. +8]\n"
; CHECK-NEXT:  .ascii "END\n"
define i64 @lo12() {
  %a = load i64, ptr getelementptr inbounds ([4 x i64], ptr @g, i64 0, i64 1), align 8
  ret i64 %a
}

; A store through one pointer and a load through another: the two addresses
; differ by an unknown amount, so the load starts a second form.
; CHECK-LABEL: HBNG_alias_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X0\n"
; CHECK-NEXT:  .ascii "send X1\n"
; CHECK-NEXT:  .ascii "M[S0 .. +8] <- X2\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X0 <- M[S1 .. +8]\n"
; CHECK-NEXT:  .ascii "END\n"
define i64 @alias(ptr %p, ptr %q, i64 %v) {
  store i64 %v, ptr %p, align 8
  %a = load i64, ptr %q, align 8
  ret i64 %a
}

; A system call is a form of its own.
; CHECK-LABEL: HBNG_opaque_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "svc\n"
; CHECK-NEXT:  .ascii "END\n"
define void @opaque(i64 %a) {
  %r = add i64 %a, 1
  call i64 asm sideeffect "svc #0", "={x0},{x0},~{memory}"(i64 %r)
  ret void
}

; "mov x29, sp" then the stur through x29: the composed form addresses the
; store from <SP> (x29 is the entry SP minus 32 once the pair is pushed).
; CHECK-LABEL: HBNG_vla_annot:
; CHECK:       .ascii "M[<SP> - 56 .. +8] <- X0\n"
; CHECK:       .ascii "<SP> <- S0\n"
; CHECK-NEXT:  .ascii "<FP> <- <SP> - 32\n"
; CHECK-NEXT:  .ascii "END\n"
; The epilogue reloads through x29 and restores SP from it: addressed from <FP>,
; the new FP being the value sent after the reload.
; CHECK-LABEL: HBNG_vla_split_0_annot:
; CHECK:       .ascii "X19 <- M[<FP> + 16 .. +8]\n"
; CHECK:       .ascii "<SP> <- <FP> + 32\n"
; CHECK-NEXT:  .ascii "<FP> <- S0\n"
; CHECK-NEXT:  .ascii "END\n"
declare void @use(ptr)
define i64 @vla(i64 %n) {
  %vla = alloca i64, i64 %n, align 8
  %fixed = alloca [4 x i64], align 8
  %p = getelementptr i64, ptr %fixed, i64 1
  store volatile i64 %n, ptr %p, align 8
  %a = load volatile i64, ptr %p, align 8
  call void @use(ptr %vla)
  ret i64 %a
}

; The table entries: instruction count with the waypoint bit (0x8000, both
; blocks end in a ret), number of sends, then the instruction index of each
; send (0 twice for the stp of compose), padded to 4 bytes.
; TBL:         .section .hbngbbt
; TBL-NEXT:    .word HBNG_compose
; TBL-NEXT:    .word HBNG_compose_annot
; TBL-NEXT:    .hword 32772
; TBL-NEXT:    .hword 2
; TBL-NEXT:    .hword 0
; TBL-NEXT:    .hword 0
; TBL-NEXT:    .p2align 2
; TBL-NEXT:    .word HBNG_lo12
; TBL-NEXT:    .word HBNG_lo12_annot
; TBL-NEXT:    .hword 32774
; TBL-NEXT:    .hword 2
; TBL-NEXT:    .hword 1
; TBL-NEXT:    .hword 3
; TBL-NEXT:    .p2align 2
