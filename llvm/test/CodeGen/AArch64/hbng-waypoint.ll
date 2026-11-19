; The block table's waypoint bit (0x8000 in the instruction count) is set when
; a block ends in an instruction the trace emits an atom for: a branch, call,
; return or isb. An isb ends its block; an svc ends its block without a
; waypoint, and so does a block that falls through into a label.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: sed 's/^; ERR-ONLY: //' %s > %t.ll
; RUN: not llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %t.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR

; CHECK-LABEL: isb_mid:
; CHECK:       {{^}}	isb{{$}}
; CHECK-NEXT:  HBNG_isb_mid_split_0:

; CHECK-LABEL: svc_mid:
; CHECK:       svc #0
; CHECK-NEXT:  //NO_APP
; CHECK-NEXT:  HBNG_svc_mid_split_0:

; CHECK-LABEL: .section .hbngbbt
; isb_mid: four instructions up to the isb, then four up to the ret.
; CHECK-NEXT:  .word HBNG_isb_mid
; CHECK-NEXT:  .word HBNG_isb_mid_annot
; CHECK-NEXT:  .hword 32772
; CHECK:       .word HBNG_isb_mid_split_0
; CHECK-NEXT:  .word HBNG_isb_mid_split_0_annot
; CHECK-NEXT:  .hword 32772
; svc_mid: the svc's block has no waypoint, the ret's has.
; CHECK:       .word HBNG_svc_mid
; CHECK-NEXT:  .word HBNG_svc_mid_annot
; CHECK-NEXT:  .hword 1
; CHECK:       .word HBNG_svc_mid_split_0
; CHECK-NEXT:  .word HBNG_svc_mid_split_0_annot
; CHECK-NEXT:  .hword 32769
; call_mid: the bl ends a block.
; CHECK:       .word HBNG_call_mid
; CHECK-NEXT:  .word HBNG_call_mid_annot
; CHECK-NEXT:  .hword 32771
; loop: the entry falls through into the loop, which ends in a conditional
; branch.
; CHECK:       .word HBNG_loop
; CHECK-NEXT:  .word HBNG_loop_annot
; CHECK-NEXT:  .hword 1
; CHECK:       .word HBNG_loop_n_1
; CHECK-NEXT:  .word HBNG_loop_n_1_annot
; CHECK-NEXT:  .hword 32773

; Both addresses of an entry are absolute 32-bit relocations.
; RELOC:       Section ({{[0-9]+}}) .rela.hbngbbt {
; RELOC-NEXT:    0x0 R_AARCH64_ABS32 .text 0x0
; RELOC-NEXT:    0x4 R_AARCH64_ABS32 .hbngannot 0x0

; ERR: HBNG: isb must be the last instruction of an inline asm statement, in function 'isb_first'

declare void @llvm.aarch64.isb(i32)
declare void @g()

define void @isb_mid(ptr %p) {
  store i64 1, ptr %p
  call void @llvm.aarch64.isb(i32 15)
  store i64 2, ptr %p
  ret void
}

define void @svc_mid() {
  call void asm sideeffect "svc #0", "~{x0},~{memory}"()
  ret void
}

define void @call_mid(ptr %p) {
  call void @g()
  store i64 2, ptr %p
  ret void
}

define void @loop(ptr %p, i64 %n) {
entry:
  br label %l
l:
  %i = phi i64 [ 0, %entry ], [ %j, %l ]
  %q = getelementptr i64, ptr %p, i64 %i
  store i64 %i, ptr %q
  %j = add i64 %i, 1
  %c = icmp ult i64 %j, %n
  br i1 %c, label %l, label %e
e:
  ret void
}

; ERR-ONLY: define void @isb_first() {
; ERR-ONLY:   call void asm sideeffect "isb; nop", ""()
; ERR-ONLY:   ret void
; ERR-ONLY: }
