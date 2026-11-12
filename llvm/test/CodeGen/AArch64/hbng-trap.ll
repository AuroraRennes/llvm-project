; Traps (brk, hlt): nothing written.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s

; brk #0xf000 ; hlt #0
; CHECK-LABEL: HBNG_traps_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; brk #0x1
; CHECK-LABEL: HBNG_trap_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "END\n"

define void @traps() {
  call void @llvm.debugtrap()
  call void @llvm.aarch64.hlt(i32 0)
  ret void
}

define void @trap() {
  call void @llvm.trap()
  unreachable
}

declare void @llvm.debugtrap()

declare void @llvm.aarch64.hlt(i32)

declare void @llvm.trap()
