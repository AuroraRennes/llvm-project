; Debug-info pseudo-instructions (DBG_VALUE, DBG_LABEL, ...) emit no code and
; annotate nothing: a -g build gives the same annotations as without -g.
; RUN: llc -mtriple=aarch64-linux-musl -mattr=+reserve-x18 -hbng -O0 < %s | FileCheck %s

; CHECK-LABEL: HBNG_label_annot:
; CHECK-NOT:   Unsupported
; CHECK:       .ascii "END\n"

define i32 @label(i32 %x) !dbg !6 {
entry:
  br label %out

out:
  call void @llvm.dbg.label(metadata !10), !dbg !11
  ret i32 %x, !dbg !11
}

declare void @llvm.dbg.label(metadata)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "label.c", directory: "/")
!3 = !{i32 2, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!6 = distinct !DISubprogram(name: "label", scope: !1, file: !1, line: 1, type: !7, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!7 = !DISubroutineType(types: !8)
!8 = !{null}
!10 = !DILabel(scope: !6, name: "out", file: !1, line: 3)
!11 = !DILocation(line: 3, column: 1, scope: !6)
