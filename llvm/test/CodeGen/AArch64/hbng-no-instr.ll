; A hbng_no_instr function is uncompiled code for HBNG: no sends, no block
; labels, no annotations, no block table entries, but its own block labels are
; still emitted, so its branches resolve. The other functions are unaffected.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 -filetype=obj < %s -o /dev/null

; CHECK-LABEL: start_c:
; CHECK-NOT:   HBNG_start_c
; CHECK-NOT:   x18
; CHECK:       .LBB0_1:
; CHECK-NOT:   x18
; CHECK:       b.ne .LBB0_1
; CHECK:       .Lfunc_end0:
define void @start_c(ptr %p, i64 %n) "hbng_no_instr" {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %a = getelementptr i64, ptr %p, i64 %i
  %v = load volatile i64, ptr %a
  call void @g(i64 %v)
  %i.next = add i64 %i, 1
  %c = icmp ne i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: user:
; CHECK:       HBNG_user:
; CHECK:       str x0, [x18]
; CHECK-NEXT:  ldr x0, [x0]

; CHECK-NOT:   HBNG_start_c
; CHECK-LABEL: .section .hbngannot
; CHECK-NEXT:  HBNG_user_annot:
; CHECK:       .section .hbngbbt
; CHECK-NEXT:  .xword HBNG_user
; CHECK-NEXT:  .xword HBNG_user_annot
; CHECK-NOT:   HBNG_start_c
define i64 @user(ptr %p) {
  %v = load i64, ptr %p
  ret i64 %v
}

declare void @g(i64)
