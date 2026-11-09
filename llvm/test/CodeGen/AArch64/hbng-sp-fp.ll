; SP and FP are tracked values: an access based on them sends nothing, an
; update by an immediate from SP or FP is computed from the annotation, a
; write of SP from another register sends its sources before it (SP itself
; cannot be stored to the STM), and any other write of FP sends the new value
; right after the instruction. A call preserves both: no send after it.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; Alloca: SP is set from a computed register, sent before the move; the
; epilogue's "mov sp, x29" is computed from FP; the reload of x29 is sent
; after the ldp; nothing is sent after the call.
; CHECK-LABEL: vla:
; CHECK:       stp x29, x30, [sp, #-32]!
; CHECK-NEXT:  str x19, [sp, #16]
; CHECK-NEXT:  mov x29, sp
; CHECK:       sub [[NEWSP:x[0-9]+]], {{x[0-9]+}}, {{x[0-9]+}}
; CHECK-NEXT:  str [[NEWSP]], [x18]
; CHECK-NEXT:  mov sp, [[NEWSP]]
; CHECK:       bl use
; CHECK-NEXT:  HBNG_vla_split_0:
; CHECK-NEXT:  str [[NEWSP]], [x18]
; CHECK-NEXT:  ldrb w0, [[[NEWSP]], #2]
; CHECK-NEXT:  mov sp, x29
; CHECK-NEXT:  ldr x19, [sp, #16]
; CHECK-NEXT:  ldp x29, x30, [sp], #32
; CHECK-NEXT:  str x29, [x18]
; CHECK-NEXT:  ret

; Over-aligned frame: "and sp, x9, #-64", with x9 sent before; the
; SP-based access after the call sends nothing.
; CHECK-LABEL: realign:
; CHECK:       sub x9, sp, #48
; CHECK-NEXT:  mov x29, sp
; CHECK-NEXT:  str x9, [x18]
; CHECK-NEXT:  and sp, x9, #0xffffffffffffffc0
; CHECK:       HBNG_realign_split_0:
; CHECK-NEXT:  ldrb w0, [sp, #1]

; x29 as a general register (no frame pointer): written by inline asm, its
; value is sent after the asm and after the epilogue's reload.
; CHECK-LABEL: clobber_fp:
; CHECK:       //APP
; CHECK-NEXT:  mov x29, x0
; CHECK-NEXT:  //NO_APP
; CHECK-NEXT:  str x29, [x18]
; CHECK:       ldr x29, [sp], #16
; CHECK-NEXT:  str x29, [x18]

; CHECK-LABEL: HBNG_vla_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "[<SP> + 8*Imm(-4)]_64 <- FP\n"
; CHECK-NEXT:  .ascii "[<SP> + 8*Imm(-4) + 8]_64 <- LR\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + Imm(-32)\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "[<SP> + 8*Imm(2)]_64 <- X19\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "FP <- SP add Imm(0)\n"
; CHECK-NEXT:  .ascii "<FP> <- <SP> + Imm(0)\n"
; CHECK:       .ascii "SP <- [[NEWSP:X[0-9]+]] add Imm(0)\n"
; CHECK-NEXT:  .ascii "<SP> <- <[[NEWSP]]> + Imm(0)\n"
; CHECK:       .ascii "LR <- Imm(0)\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_vla_split_0_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X19\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "W0 <- [<[[NEWSP]]> + 1*Imm(2)]_8\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "SP <- FP add Imm(0)\n"
; CHECK-NEXT:  .ascii "<SP> <- <FP> + Imm(0)\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "X19 <- [<SP> + 8*Imm(2)]_64\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "FP <- [<SP> + Imm(0)]_64\n"
; CHECK-NEXT:  .ascii "LR <- [<SP> + Imm(0) + 8]_64\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + Imm(32)\n"
; CHECK-NEXT:  .ascii "<FP> <- SEND\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "send X29\n"
; CHECK-NEXT:  .ascii "@24\n"
; CHECK-NEXT:  .ascii "END\n"
; CHECK-LABEL: HBNG_realign_annot:
; CHECK:       .ascii "X9 <- SP sub Imm(48)\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "FP <- SP add Imm(0)\n"
; CHECK-NEXT:  .ascii "<FP> <- <SP> + Imm(0)\n"
; CHECK-NEXT:  .ascii "@12\n"
; CHECK-NEXT:  .ascii "send X9\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "SP <- X9 log Imm({{[0-9]+}})\n"
; CHECK-NEXT:  .ascii "<SP> <- <X9> and Imm(-64)\n"
; CHECK-LABEL: HBNG_clobber_fp_annot:
; CHECK:       .ascii "Unsupported: INLINEASM mov\n"
; CHECK-NEXT:  .ascii "<FP> <- SEND\n"
; CHECK:       .ascii "FP <- [<SP> + Imm(0)]_64\n"
; CHECK-NEXT:  .ascii "<SP> <- <SP> + Imm(16)\n"
; CHECK-NEXT:  .ascii "<FP> <- SEND\n"

target triple = "aarch64-unknown-linux-musl"

define i32 @vla(i32 %n) "frame-pointer"="all" {
entry:
  %0 = zext i32 %n to i64
  %vla = alloca i8, i64 %0, align 1
  call void @use(ptr nonnull %vla)
  %arrayidx = getelementptr inbounds nuw i8, ptr %vla, i64 2
  %1 = load i8, ptr %arrayidx, align 1
  %conv = zext i8 %1 to i32
  ret i32 %conv
}

define i64 @realign() "frame-pointer"="all" {
entry:
  %b = alloca [8 x i8], align 64
  call void @use(ptr nonnull %b)
  %arrayidx = getelementptr inbounds nuw i8, ptr %b, i64 1
  %0 = load i8, ptr %arrayidx, align 1
  %conv = zext i8 %0 to i64
  ret i64 %conv
}

define void @clobber_fp(i64 %v) "frame-pointer"="none" {
entry:
  call void asm sideeffect "mov x29, $0", "r,~{x29}"(i64 %v)
  ret void
}

declare void @use(ptr)
