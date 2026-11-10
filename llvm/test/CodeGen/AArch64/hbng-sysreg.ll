; System registers and barriers: an mrs gives an untagged value (the thread
; pointer, FPCR, counters are not data the program read or received), and a
; barrier moves no data.
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

define ptr @tp() {
  %p = call ptr @llvm.thread.pointer.p0()
  ret ptr %p
}

define void @fence() {
  fence seq_cst
  ret void
}

declare ptr @llvm.thread.pointer.p0()
