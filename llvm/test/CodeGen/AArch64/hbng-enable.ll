; The HBNG passes run only with -hbng, and the sends store through x18, which
; must then be reserved (-ffixed-x18, +reserve-x18).
; RUN: llc -mtriple=aarch64-linux-musl -O2 < %s | FileCheck %s --check-prefix=OFF
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O2 < %s | FileCheck %s --check-prefix=ON
; RUN: not llc -mtriple=aarch64-linux-musl -hbng -O2 < %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR

; OFF-LABEL: load:
; OFF-NOT:     x18
; OFF-NOT:     HBNG_
; OFF:         .Lfunc_end0:
; OFF-NOT:     .hbngannot
; OFF-NOT:     .hbngbbt

; ON-LABEL: load:
; ON-NEXT:     .cfi_startproc
; ON-NEXT:   // %bb.0:
; ON-NEXT:   HBNG_load:
; ON-NEXT:     str x0, [x18]
; ON-NEXT:     ldr x0, [x0]
; ON:          .section .hbngannot
; ON:          .section .hbngbbt

; ERR: HBNG: x18 holds the STM address and must be reserved (-ffixed-x18), in function 'load'

define i64 @load(ptr %p) {
  %v = load i64, ptr %p
  ret i64 %v
}
