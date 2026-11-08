; The sends are inserted before branch relaxation, so that it measures the code
; with them. With tbz limited to +-16 instructions (5 bits), the 10 loads alone
; fit but not with their 10 sends: relaxation must turn the tbz into tbnz to the
; next block + b, and the result must assemble. The split after the tbnz must
; keep the next block's label.
; RUN: llc -mtriple=aarch64-linux-musl -O2 -aarch64-tbz-offset-bits=5 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-linux-musl -O2 -aarch64-tbz-offset-bits=5 -filetype=obj < %s -o /dev/null

; CHECK-LABEL: far:
; CHECK:       tbnz w1, #0, [[BODY:.LBB0_[0-9]+]]
; CHECK-NEXT:  HBNG_far_split_0:
; CHECK-NEXT:  b [[SKIP:.LBB0_[0-9]+]]
; CHECK:       [[BODY]]:
; CHECK-COUNT-10: str x0, [x26]
; CHECK:       [[SKIP]]:
define void @far(ptr %p, i64 %x) {
entry:
  %bit = and i64 %x, 1
  %c = icmp eq i64 %bit, 0
  br i1 %c, label %skip, label %body
body:
  %a0 = getelementptr i64, ptr %p, i64 0
  %v0 = load volatile i64, ptr %a0
  %a1 = getelementptr i64, ptr %p, i64 1
  %v1 = load volatile i64, ptr %a1
  %a2 = getelementptr i64, ptr %p, i64 2
  %v2 = load volatile i64, ptr %a2
  %a3 = getelementptr i64, ptr %p, i64 3
  %v3 = load volatile i64, ptr %a3
  %a4 = getelementptr i64, ptr %p, i64 4
  %v4 = load volatile i64, ptr %a4
  %a5 = getelementptr i64, ptr %p, i64 5
  %v5 = load volatile i64, ptr %a5
  %a6 = getelementptr i64, ptr %p, i64 6
  %v6 = load volatile i64, ptr %a6
  %a7 = getelementptr i64, ptr %p, i64 7
  %v7 = load volatile i64, ptr %a7
  %a8 = getelementptr i64, ptr %p, i64 8
  %v8 = load volatile i64, ptr %a8
  %a9 = getelementptr i64, ptr %p, i64 9
  %v9 = load volatile i64, ptr %a9
  br label %skip
skip:
  ret void
}
