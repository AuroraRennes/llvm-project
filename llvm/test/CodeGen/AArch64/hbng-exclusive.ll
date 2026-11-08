; No send between a load-exclusive and its store-exclusive: an explicit store
; there may clear the exclusive monitor, so the store-exclusive could fail on
; every retry. The load-exclusive's send stays before it, the store-exclusive's
; goes right after it.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,-lse -hbng -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18,-lse -hbng -O0 < %s | FileCheck %s

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
