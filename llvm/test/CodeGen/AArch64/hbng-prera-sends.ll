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

; Two stores through the same base: one send, before the first (the base is
; not written in between). Nothing separates the stores, which become a pair.
; CHECK-LABEL: pair:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  stp x1, x2, [x0]
define void @pair(ptr %p, i64 %a, i64 %b) {
  store i64 %a, ptr %p
  %q = getelementptr i64, ptr %p, i64 1
  store i64 %b, ptr %q
  ret void
}

; A W register offset is sent as its X register, after the base. The two
; adjacent sends are one pair, the base at the higher address (received
; first).
; CHECK-LABEL: woffset:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  stp x1, x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldr x0, [x0, w1, sxtw #3]
; ANNOT-LABEL: HBNG_woffset_annot:
; ANNOT:       .ascii "send X0\n"
; ANNOT-NEXT:  .ascii "send X1\n"
define i64 @woffset(ptr %p, i32 %i) {
  %q = getelementptr i64, ptr %p, i32 %i
  %v = load i64, ptr %q
  ret i64 %v
}

; A W register offset truncated from a live 64-bit register: that register
; is sent, with no copy of the offset into another one.
; CHECK-LABEL: trunc_index:
; CHECK-NOT:   mov w
; CHECK:       str x1, {{\[}}[[P:x[0-9]+]]{{\]}}
; CHECK-NEXT:  str x2, [{{x[0-9]+}}, w1, sxtw #3]
define i64 @trunc_index(ptr %p, i64 %n, i64 %v) {
  %i = trunc i64 %n to i32
  %q = getelementptr i64, ptr %p, i32 %i
  store i64 %v, ptr %q
  %r = add i64 %n, 1
  ret i64 %r
}

; A pair access refers to its base once per data register, but all pair with
; one send.
; CHECK-LABEL: ldp:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldp x0, x1, [x0]
define i128 @ldp(ptr %p) "target-features"="+lse2" {
  %v = load atomic i128, ptr %p monotonic, align 16
  ret i128 %v
}

; An atomic expanded before instruction selection: the load-exclusive's send
; before it, which the store-exclusive also pairs with (same base), no store
; between the two exclusives.
; CHECK-LABEL: fetch_add:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK:       str [[B:x[0-9]+]], {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldaxr {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NOT:   str
; CHECK:       stlxr [[S:w[0-9]+]], {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NEXT:  cbnz [[S]]
define i64 @fetch_add(ptr %p, i64 %v) {
  %r = atomicrmw add ptr %p, i64 %v seq_cst
  ret i64 %r
}

; A store-exclusive whose base is not sent before it sends it right after
; itself.
; CHECK-LABEL: stlxr:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK:       stlxr {{w[0-9]+}}, x1, {{\[}}[[B:x[0-9]+]]{{\]}}
; CHECK-NEXT:  str [[B]], {{\[}}[[P]]{{\]}}
define i32 @stlxr(ptr %p, i64 %v) {
  %s = call i32 @llvm.aarch64.stlxr.p0(i64 %v, ptr elementtype(i64) %p)
  ret i32 %s
}

; The epilogue's reload of FP is sent after it (a fallback, through a free
; register: x9-x17 are dead at a return).
; CHECK-LABEL: frame:
; CHECK:       ldp x29, x30, [sp, #16]
; CHECK-NEXT:  adrp [[S:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str x29, {{\[}}[[S]]{{\]}}
; CHECK-NEXT:  add sp, sp, #32
; CHECK-NEXT:  ret
; ANNOT-LABEL: HBNG_frame_annot:
; ANNOT:       .ascii "send X29\n"
; ANNOT-NEXT:  .ascii "<FP> <- <X29>\n"
declare void @g(ptr)
define void @frame() "frame-pointer"="all" {
  %a = alloca i64
  call void @g(ptr %a)
  ret void
}

; A fallback reuses a register still holding the page: the FP reload is sent
; through the load's page register, with no second adrp.
; CHECK-LABEL: reuse:
; CHECK:       adrp [[P:x[0-9]+]], __hbng_chan
; CHECK-NEXT:  str x0, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ldr x0, [x0]
; CHECK-NEXT:  ldp x29, x30, [sp], #16
; CHECK-NEXT:  str x29, {{\[}}[[P]]{{\]}}
; CHECK-NEXT:  ret
define i64 @reuse(ptr %p) "frame-pointer"="all" {
  %v = load i64, ptr %p
  ret i64 %v
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
