; A prefetch moves no data: no line.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; prfm pldl1keep, [x0]
; CHECK-LABEL: HBNG_prefetch_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define void @prefetch(ptr %p) {
entry:
  call void @llvm.prefetch.p0(ptr %p, i32 0, i32 3, i32 1)
  ret void
}
