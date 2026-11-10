; Every instruction of a block has its group, "@<offset>" and then its lines,
; empty groups included, and every block is closed by END, including one that
; falls through into the next. A pseudo expanded at emission has one group per
; instruction it becomes.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 -aarch64-min-jump-table-entries=4 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 -aarch64-min-jump-table-entries=4 < %s | FileCheck %s --check-prefix=ASM

; The load block falls through into the return.
; ASM-LABEL:   HBNG_fall_n_1:
; ASM-NEXT:    str x1, [x18]
; ASM-NEXT:    ldr w8, [x1]
; ASM-NEXT:    add w0, w8, w0
; ASM-NEXT:    .LBB{{[0-9_]+}}:
; ASM-NEXT:    HBNG_fall_n_2:
; CHECK-LABEL: HBNG_fall_n_1_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "send X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "X8 <- M[<X1> .. +4]\n"
; CHECK-NEXT:  .ascii "@8\n"
; CHECK-NEXT:  .ascii "X0 <- X8 | X0\n"
; CHECK-NEXT:  .ascii "END\n"
define i32 @fall(i32 %x, ptr %p) {
entry:
  %c = icmp eq i32 %x, 0
  br i1 %c, label %zero, label %load
load:
  %v = load i32, ptr %p
  %s = add i32 %v, %x
  br label %zero
zero:
  %r = phi i32 [ 0, %entry ], [ %s, %load ]
  ret i32 %r
}

; The jump-table dispatch (adr, ldrb, add) is one pseudo until emission: three
; groups, then the br's.
; ASM-LABEL:   HBNG_table_n_1:
; ASM:         adr x10, .LBB1_2
; ASM-NEXT:    ldrb w11, [x9, x8]
; ASM-NEXT:    add x10, x10, x11, lsl #2
; ASM-NEXT:    br x10
; CHECK-LABEL: HBNG_table_n_1_annot:
; CHECK:       .ascii "@12\n"
; CHECK-NEXT:  .ascii "X9 <- X9\n"
; CHECK-NEXT:  .ascii "@16\n"
; CHECK-NEXT:  .ascii "X10 <- 0\n"
; CHECK-NEXT:  .ascii "@20\n"
; CHECK-NEXT:  .ascii "X11 <- 0\n"
; CHECK-NEXT:  .ascii "@24\n"
; CHECK-NEXT:  .ascii "X10 <- X10 | X11\n"
; CHECK-NEXT:  .ascii "@28\n"
; CHECK-NEXT:  .ascii "END\n"

declare void @f0()
declare void @f1()
declare void @f2()
declare void @f3()

define void @table(i32 %x) {
entry:
  switch i32 %x, label %out [
    i32 0, label %a
    i32 1, label %b
    i32 2, label %c
    i32 3, label %d
  ]
a:
  call void @f0()
  br label %out
b:
  call void @f1()
  br label %out
c:
  call void @f2()
  br label %out
d:
  call void @f3()
  br label %out
out:
  ret void
}
