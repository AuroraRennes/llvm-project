; Sends decided before register allocation (-hbng-prera-sends): stores to the
; page of __hbng_chan through any register. The pass after register
; allocation checks that every value an annotation refers to pairs with a
; send (nearest preceding send of the register in the block, not written in
; between) and adds the missing ones (fallbacks).
; RUN: llc -mtriple=aarch64-linux-musl -mattr=-lse -hbng -hbng-prera-sends -O2 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=-lse -hbng -hbng-prera-sends -O2 < %s | FileCheck %s --check-prefix=ANNOT
; RUN: llc -mtriple=aarch64-linux-musl -mattr=-lse -hbng -hbng-prera-sends -O0 < %s | FileCheck %s --check-prefix=O0

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

; SP written from a register (a variable-size alloca): its source is sent
; before the write, by a fallback (the write only exists after register
; allocation).
; CHECK-LABEL: vla:
; CHECK:       sub [[N:x[0-9]+]], {{x[0-9]+}}, {{x[0-9]+}}
; CHECK-NEXT:  adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str [[N]], {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  mov sp, [[N]]
; ANNOT-LABEL: HBNG_vla_annot:
; ANNOT:       .ascii "send X0\n"
; ANNOT-NEXT:  .ascii "@{{[0-9]+}}\n"
; ANNOT-NEXT:  .ascii "SP <- X0\n"
; ANNOT-NEXT:  .ascii "<SP> <- <X0>\n"
declare void @g(ptr)
define void @vla(i64 %n) "frame-pointer"="all" {
  %a = alloca i8, i64 %n
  call void @g(ptr %a)
  ret void
}

; At -O0 a compare-and-swap stays a pseudo until after register allocation:
; its exclusives are sent by fallbacks, the load-exclusive's before it and
; the store-exclusive's after it.
; O0-LABEL: cas:
; O0:       adrp [[P:x[0-9]+]], __hbng_chan
; O0-NEXT:  str [[B:x[0-9]+]], {{\[}}[[P]]{{\]}}
; O0-NEXT:  ldaxr {{w[0-9]+}}, {{\[}}[[B]]{{\]}}
; O0-NOT:   str
; O0:       stlxr [[S:w[0-9]+]], {{w[0-9]+}}, {{\[}}[[B]]{{\]}}
; O0-NEXT:  adrp [[Q:x[0-9]+]], __hbng_chan
; O0-NEXT:  str [[B]], {{\[}}[[Q]]{{\]}}
; O0-NEXT:  cbnz [[S]]
define i32 @cas(ptr %p, i32 %old, i32 %new) {
  %pair = cmpxchg ptr %p, i32 %old, i32 %new seq_cst seq_cst
  %r = extractvalue { i32, i1 } %pair, 0
  ret i32 %r
}
