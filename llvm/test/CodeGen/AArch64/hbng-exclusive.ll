; No send between a load-exclusive and its store-exclusive: an explicit store
; there may clear the exclusive monitor, so the store-exclusive could fail on
; every retry. The load-exclusive's send stays before it, the store-exclusive's
; goes right after it.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,-lse -hbng -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,-lse -hbng -O0 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,-lse -hbng -O2 < %s | FileCheck %s --check-prefix=ANNOT

; CHECK-LABEL: fetch_add:
; CHECK:       ldaxr {{[wx][0-9]+}}, {{\[}}[[B:x[0-9]+]]{{\]}}
; CHECK-NOT:   x18
; CHECK:       stlxr [[S:w[0-9]+]], {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NEXT:  str [[B]], [x18]
; CHECK-NEXT:  cbnz [[S]]
define i64 @fetch_add(ptr %p, i64 %v) {
  %r = atomicrmw add ptr %p, i64 %v seq_cst
  ret i64 %r
}

; CHECK-LABEL: cas:
; CHECK:       ldaxr {{w[0-9]+}}, {{\[}}[[B:x[0-9]+]]{{\]}}
; CHECK-NOT:   x18
; CHECK:       stlxr [[S:w[0-9]+]], {{w[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NEXT:  str [[B]], [x18]
; CHECK-NEXT:  cbnz [[S]]
define i32 @cas(ptr %p, i32 %old, i32 %new) {
  %pair = cmpxchg ptr %p, i32 %old, i32 %new seq_cst seq_cst
  %r = extractvalue { i32, i1 } %pair, 0
  ret i32 %r
}

; Pairs: the base is sent twice, with one STP.
; CHECK-LABEL: cas128:
; CHECK:       ldaxp {{x[0-9]+}}, {{x[0-9]+}}, {{\[}}[[B:x[0-9]+]]{{\]}}
; CHECK-NOT:   x18
; CHECK:       stlxp [[S:w[0-9]+]], {{x[0-9]+}}, {{x[0-9]+}}, {{\[}}[[B]]{{\]}}
; CHECK-NEXT:  stp [[B]], [[B]], [x18]
; CHECK-NEXT:  cbnz [[S]]
define i128 @cas128(ptr %p, i128 %old, i128 %new) {
  %pair = cmpxchg ptr %p, i128 %old, i128 %new seq_cst seq_cst
  %r = extractvalue { i128, i1 } %pair, 0
  ret i128 %r
}

; Storing the zero register keeps the bytes (each keeps its tag or gets none,
; which is keeping it): the store-exclusive writes only the status.
; ANNOT-LABEL: xchg0:
; ANNOT:       stlxr {{w[0-9]+}}, wzr, {{\[}}x{{[0-9]+}}{{\]}}
; ANNOT-LABEL: HBNG_xchg0_n_1_annot:
; ANNOT:       .ascii "@8\n"
; ANNOT-NEXT:  .ascii "X{{[0-9]+}} <- 0\n"
; ANNOT-NEXT:  .ascii "@12\n"
define i32 @xchg0(ptr %p) {
  %r = atomicrmw xchg ptr %p, i32 0 seq_cst
  ret i32 %r
}
