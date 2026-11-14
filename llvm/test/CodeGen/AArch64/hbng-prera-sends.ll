; Sends decided before register allocation (-hbng-prera-sends): stores to the
; page of __hbng_chan through any register.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=-lse -hbng -hbng-prera-sends -O2 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=-lse -hbng -hbng-prera-sends -O2 < %s | FileCheck %s --check-prefix=ANNOT

; One load: the page once, then the base, before the load.
; CHECK-LABEL: load:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK:       str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldr x0, [x0]
; ANNOT-LABEL: HBNG_load_annot:
; ANNOT:       .ascii "send X0\n"
define i64 @load(ptr %p) {
  %v = load i64, ptr %p
  ret i64 %v
}

; Each store has its send, right before it, through the same page.
; CHECK-LABEL: pair:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  str x1, [x0]
; CHECK-NEXT:  str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  str x2, [x0, #8]
define void @pair(ptr %p, i64 %a, i64 %b) {
  store i64 %a, ptr %p
  %q = getelementptr i64, ptr %p, i64 1
  store i64 %b, ptr %q
  ret void
}

; A W register offset is sent as its X register, after the base.
; CHECK-LABEL: woffset:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK:       str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  str x1, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldr x0, [x0, w1, sxtw #3]
; ANNOT-LABEL: HBNG_woffset_annot:
; ANNOT:       .ascii "send X0\n"
; ANNOT:       .ascii "send X1\n"
define i64 @woffset(ptr %p, i32 %i) {
  %q = getelementptr i64, ptr %p, i32 %i
  %v = load i64, ptr %q
  ret i64 %v
}

; An atomic expanded before instruction selection: the load-exclusive's send
; before it, the store-exclusive's right after it, no store between the two
; exclusives.
; CHECK-LABEL: fetch_add:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK:       str [[B:x[0-9]+]], {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldaxr {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NOT:   str
; CHECK:       stlxr [[S:w[0-9]+]], {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NEXT:  str [[B]], {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  cbnz [[S]]
define i64 @fetch_add(ptr %p, i64 %v) {
  %r = atomicrmw add ptr %p, i64 %v seq_cst
  ret i64 %r
}
