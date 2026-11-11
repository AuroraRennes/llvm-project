; CRC32 and CRC32C: the new checksum from the old one and the data, a union.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,+crc -hbng -O2 < %s | FileCheck %s

; crc32b w0, w0, w1
; CHECK-LABEL: HBNG_crc32b_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"
; crc32cx w0, w0, x1
; CHECK-LABEL: HBNG_crc32cx_annot:
; CHECK-NEXT:  .ascii "@0\n"
; CHECK-NEXT:  .ascii "X0 <- X0 | X1\n"
; CHECK-NEXT:  .ascii "@4\n"
; CHECK-NEXT:  .ascii "END\n"

define i32 @crc32b(i32 %crc, i32 %data) {
entry:
  %r = call i32 @llvm.aarch64.crc32b(i32 %crc, i32 %data)
  ret i32 %r
}

define i32 @crc32cx(i32 %crc, i64 %data) {
entry:
  %r = call i32 @llvm.aarch64.crc32cx(i32 %crc, i64 %data)
  ret i32 %r
}
