; Loads and stores: the HBNG annotations describe the right address, width and
; registers, and the STM sends carry the address registers those annotations
; refer to (the base unless it is SP, then the register offset; once per data
; register).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s --check-prefix=SEND
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s --check-prefix=ANNOT

; Store-exclusive: the base is the last operand, the status register is
; written with an untainted value, and the send comes after the instruction
; (see hbng-exclusive.ll).
; SEND-LABEL: excl_add:
; SEND:       str x8, [x18]
; SEND-NEXT:  ldaxr x0, [x8]
; SEND:       stlxr w10, x9, [x8]
; SEND-NEXT:  str x8, [x18]
; ANNOT-LABEL: HBNG_excl_add_n_1_annot:
; ANNOT-NEXT:  .ascii "X0 <- [<X8> + Imm(0)]_64\n"
; ANNOT-NEXT:  .ascii "X9 <- X0 add X1\n"
; ANNOT-NEXT:  .ascii "[<X8> + Imm(0)]_64 <- X9\n"
; ANNOT-NEXT:  .ascii "W10 <- Imm(0)\n"

; Register offsets: shift by log2 of the access size; a W offset is sent as
; its X register, in one STP with the base.
; SEND-LABEL: reg_x:
; SEND:       stp x1, x0, [x18]
; SEND-NEXT:  ldr x0, [x0, x1, lsl #3]
; SEND-LABEL: reg_w:
; SEND:       stp x1, x0, [x18]
; SEND-NEXT:  ldr w0, [x0, w1, sxtw #2]
; ANNOT-LABEL: HBNG_reg_x_annot:
; ANNOT-NEXT:  .ascii "X0 <- [<X0> + <X1> uxtx 3]_64\n"
; ANNOT-LABEL: HBNG_reg_w_annot:
; ANNOT-NEXT:  .ascii "W0 <- [<X0> + <W1> sxtw 2]_32\n"

; Sign-extending byte load: one byte, immediate scaled by 1.
; SEND-LABEL: ldrsb_imm:
; SEND:       str x0, [x18]
; SEND-NEXT:  ldrsb w0, [x0, #5]
; ANNOT-LABEL: HBNG_ldrsb_imm_annot:
; ANNOT-NEXT:  .ascii "W0 <- [<X0> + 1*Imm(5)]_8\n"

; Pair: the base is sent once per register; the second register is 8 bytes on.
; SEND-LABEL: pair:
; SEND:       stp x0, x0, [x18]
; SEND-NEXT:  ldp x8, x9, [x0]
; ANNOT-LABEL: HBNG_pair_annot:
; ANNOT-NEXT:  .ascii "X8 <- [<X0> + 8*Imm(0)]_64\n"
; ANNOT-NEXT:  .ascii "X9 <- [<X0> + 8*Imm(0) + 8]_64\n"

; Post-indexed: the address is the base. The write-back leaves the base's tag
; unchanged and the base is sent again at its next access: no line.
; SEND-LABEL: copy_post:
; SEND:       str x1, [x18]
; SEND-NEXT:  ldr x8, [x1], #8
; SEND:       str x0, [x18]
; SEND-NEXT:  str x8, [x0], #8
; ANNOT-LABEL: HBNG_copy_post_n_1_annot:
; ANNOT-NEXT:  .ascii "X8 <- [<X1> + Imm(0)]_64\n"
; ANNOT-NEXT:  .ascii "X2 <- X2 ari Imm(1)\n"
; ANNOT-NEXT:  .ascii "[<X0> + Imm(0)]_64 <- X8\n"
; ANNOT-NEXT:  .ascii "END\n"

; 128-bit post-indexed load: the base is sent, not the loaded Q register.
; SEND-LABEL: vmax_post:
; SEND:       str x0, [x18]
; SEND-NEXT:  ldr q0, [x0]
; SEND:       str x0, [x18]
; SEND-NEXT:  ldr q1, [x0], #16
; ANNOT:       .ascii "Q1 <- [<X0> + Imm(0)]_128\n"
; ANNOT-NOT:   X0 <-

; Pre-indexed stack spill: byte offset, unscaled; post-indexed reload: the
; address is SP itself. SP is tracked: no send, and the write-back updates its
; value in bytes.
; SEND-LABEL: spill_pre:
; SEND-NOT:    x18
; SEND:        .Lfunc_end
; ANNOT-LABEL: HBNG_spill_pre_annot:
; ANNOT-NEXT:  .ascii "[<SP> + Imm(-16)]_64 <- LR\n"
; ANNOT-NEXT:  .ascii "<SP> <- <SP> + Imm(-16)\n"
; ANNOT-LABEL: HBNG_spill_pre_split_0_annot:
; ANNOT-NEXT:  .ascii "LR <- [<SP> + Imm(0)]_64\n"
; ANNOT-NEXT:  .ascii "<SP> <- <SP> + Imm(16)\n"

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-linux-musl"

; Function Attrs: mustprogress norecurse nounwind willreturn uwtable
define dso_local i64 @excl_add(ptr nofree noundef captures(none) %p, i64 noundef %v) local_unnamed_addr {
entry:
  %0 = atomicrmw add ptr %p, i64 %v seq_cst, align 8
  ret i64 %0
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define dso_local i64 @reg_x(ptr nofree noundef readonly captures(none) %p, i64 noundef %i) local_unnamed_addr {
entry:
  %arrayidx = getelementptr inbounds [8 x i8], ptr %p, i64 %i
  %0 = load i64, ptr %arrayidx, align 8
  ret i64 %0
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define dso_local i32 @reg_w(ptr nofree noundef readonly captures(none) %p, i32 noundef %i) local_unnamed_addr {
entry:
  %idxprom = sext i32 %i to i64
  %arrayidx = getelementptr inbounds [4 x i8], ptr %p, i64 %idxprom
  %0 = load i32, ptr %arrayidx, align 4
  ret i32 %0
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define dso_local range(i32 -128, 128) i32 @ldrsb_imm(ptr nofree noundef readonly captures(none) %p) local_unnamed_addr {
entry:
  %arrayidx = getelementptr inbounds nuw i8, ptr %p, i64 5
  %0 = load i8, ptr %arrayidx, align 1
  %conv = sext i8 %0 to i32
  ret i32 %conv
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define dso_local i64 @pair(ptr nofree noundef readonly captures(none) %p) local_unnamed_addr {
entry:
  %0 = load i64, ptr %p, align 8
  %arrayidx1 = getelementptr inbounds nuw i8, ptr %p, i64 8
  %1 = load i64, ptr %arrayidx1, align 8
  %add = add nsw i64 %1, %0
  ret i64 %add
}

; Function Attrs: nounwind uwtable
define dso_local void @copy_post(ptr nofree noundef writeonly captures(none) %d, ptr nofree noundef readonly captures(none) %s, i64 noundef %n) local_unnamed_addr {
entry:
  %tobool.not4 = icmp eq i64 %n, 0
  br i1 %tobool.not4, label %while.end, label %while.body

while.body:                                       ; preds = %entry, %while.body
  %n.addr.07 = phi i64 [ %dec, %while.body ], [ %n, %entry ]
  %s.addr.06 = phi ptr [ %add.ptr1, %while.body ], [ %s, %entry ]
  %d.addr.05 = phi ptr [ %add.ptr, %while.body ], [ %d, %entry ]
  %dec = add nsw i64 %n.addr.07, -1
  %0 = load i64, ptr %s.addr.06, align 8
  store i64 %0, ptr %d.addr.05, align 8
  %add.ptr = getelementptr inbounds nuw i8, ptr %d.addr.05, i64 8
  %add.ptr1 = getelementptr inbounds nuw i8, ptr %s.addr.06, i64 8
  tail call void asm sideeffect "", "~{memory}"()
  %tobool.not = icmp eq i64 %dec, 0
  br i1 %tobool.not, label %while.end, label %while.body

while.end:                                        ; preds = %while.body, %entry
  ret void
}

; Function Attrs: nounwind uwtable
define dso_local <4 x i32> @vmax_post(ptr nofree noundef readonly captures(none) %p, i64 noundef %n) local_unnamed_addr {
entry:
  %0 = load <4 x i32>, ptr %p, align 16
  %cmp5 = icmp sgt i64 %n, 0
  br i1 %cmp5, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.body, %entry
  %m.0.lcssa = phi <4 x i32> [ %0, %entry ], [ %add.i, %for.body ]
  ret <4 x i32> %m.0.lcssa

for.body:                                         ; preds = %entry, %for.body
  %i.08 = phi i64 [ %inc, %for.body ], [ 0, %entry ]
  %m.07 = phi <4 x i32> [ %add.i, %for.body ], [ %0, %entry ]
  %p.addr.06 = phi ptr [ %incdec.ptr, %for.body ], [ %p, %entry ]
  %1 = load <4 x i32>, ptr %p.addr.06, align 16
  %add.i = add <4 x i32> %1, %m.07
  %incdec.ptr = getelementptr inbounds nuw i8, ptr %p.addr.06, i64 16
  tail call void asm sideeffect "", "~{memory}"()
  %inc = add nuw nsw i64 %i.08, 1
  %exitcond.not = icmp eq i64 %inc, %n
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body
}

declare void @use(ptr)

define void @spill_pre() {
entry:
  %a = alloca i64
  call void @use(ptr %a)
  ret void
}
