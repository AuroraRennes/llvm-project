# REQUIRES: aarch64-registered-target, ld.lld
## llvm-hbng-postlink sorts the block table by address, adds its index and the
## format version, and fails on each table that does not describe the code.

# RUN: echo 'SECTIONS { . = 0x10000; .text : { *(.text) } . = 0x20000; \
# RUN:   .hbngannot : { *(.hbngannot) } .hbngbbt : { *(.hbngbbt) } }' > %t.lds

## A good table, written out of order.
# RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
# RUN: ld.lld -T %t.lds %t.o -o %t
# RUN: llvm-hbng-postlink %t -o %t.out
# RUN: llvm-readobj -x .hbngbbt -x .hbngidx -x .hbnginfo %t.out | FileCheck %s
## In place by default; a second run refuses the file.
# RUN: cp %t %t.inplace
# RUN: llvm-hbng-postlink %t.inplace
# RUN: cmp %t.inplace %t.out
# RUN: not llvm-hbng-postlink %t.inplace 2>&1 | FileCheck %s --check-prefix=TWICE
## Not a linked executable.
# RUN: not llvm-hbng-postlink %t.o 2>&1 | FileCheck %s --check-prefix=OBJ

# CHECK:      Hex dump of section '.hbngbbt':
## f: 2 instructions, no waypoint, then two nops before the loop; loop: 3
## instructions ending in b.lt; call: bl g, with one send at 0; noreturn: bl
## stop, then udf; g: ret; stop: b stop.
# CHECK-NEXT: 0x00020008 0c000100 00000200 02000000 1c000100
# CHECK-NEXT: 0x00020018 00000200 03800000 28000100 00000200
# CHECK-NEXT: 0x00020028 01800100 00000000 2c000100 00000200
# CHECK-NEXT: 0x00020038 01800000 34000100 00000200 01800000
# CHECK-NEXT: 0x00020048 38000100 00000200 01800000
# CHECK:      Hex dump of section '.hbngidx':
# CHECK-NEXT: 0x00000000 0c000100 00000000 1c000100 0c000000
# CHECK-NEXT: 0x00000010 28000100 18000000 2c000100 28000000
# CHECK-NEXT: 0x00000020 34000100 34000000 38000100 40000000
# CHECK:      Hex dump of section '.hbnginfo':
# CHECK-NEXT: 0x00000000 0100

# TWICE: error: {{.*}}: already has .hbngidx
# OBJ:   error: {{.*}}: not a linked executable

## One failing check per case.
# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym OVERLAP=1 %s -o %t.overlap.o
# RUN: ld.lld -T %t.lds %t.overlap.o -o %t.overlap
# RUN: not llvm-hbng-postlink %t.overlap -o /dev/null 2>&1 | FileCheck %s --check-prefix=OVERLAP
# OVERLAP: error: {{.*}}: blocks at 0x0001001c (f+0x10) and 0x00010028 (call) overlap

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym OUTSIDE=1 %s -o %t.outside.o
# RUN: ld.lld -T %t.lds %t.outside.o -o %t.outside
# RUN: not llvm-hbng-postlink %t.outside -o /dev/null 2>&1 | FileCheck %s --check-prefix=OUTSIDE
# OUTSIDE: error: {{.*}}: block at 0x00020000 is not inside an executable section{{$}}

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym ANNOT=1 %s -o %t.annot.o
# RUN: ld.lld -T %t.lds %t.annot.o -o %t.annot
# RUN: not llvm-hbng-postlink %t.annot -o /dev/null 2>&1 | FileCheck %s --check-prefix=ANNOT
# ANNOT: error: {{.*}}: block at 0x00010034 (g): annotations at 0x10034 outside .hbngannot

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym WAYPOINT=1 %s -o %t.waypoint.o
# RUN: ld.lld -T %t.lds %t.waypoint.o -o %t.waypoint
# RUN: not llvm-hbng-postlink %t.waypoint -o /dev/null 2>&1 | FileCheck %s --check-prefix=WAYPOINT
# WAYPOINT: error: {{.*}}: block at 0x0001000c (f) has the waypoint bit but ends in 0xd2800022 at 0x00010010 (f+0x4)
# WAYPOINT: error: {{.*}}: block at 0x00010034 (g) has no waypoint bit but ends in 0xd65f03c0 at 0x00010034 (g)

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym CONTINUE=1 %s -o %t.continue.o
# RUN: ld.lld -T %t.lds %t.continue.o -o %t.continue
# RUN: not llvm-hbng-postlink %t.continue -o /dev/null 2>&1 | FileCheck %s --check-prefix=CONTINUE
# CONTINUE: error: {{.*}}: block at 0x0001001c (f+0x10) can continue but is not followed by an entry at 0x00010028 (call)
# CONTINUE: error: {{.*}}: code without an entry from 0x00010028 (call) to 0x1002c

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym UNCOVERED=1 %s -o %t.uncovered.o
# RUN: ld.lld -T %t.lds %t.uncovered.o -o %t.uncovered
# RUN: not llvm-hbng-postlink %t.uncovered -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNCOVERED
# UNCOVERED: error: {{.*}}: code without an entry from 0x00010000 (_start) to 0x10008

# RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym SKIP=1 %s -o %t.skip.o
# RUN: ld.lld -T %t.lds %t.skip.o -o %t.skip
# RUN: not llvm-hbng-postlink %t.skip -o /dev/null 2>&1 | FileCheck %s --check-prefix=SKIP
# SKIP: error: {{.*}}: skipped range at 0x00010000 (_start) overlaps block at 0x0001000c (f)

.macro entry block, annot, count, waypoint, sends:vararg
  .pushsection .hbngbbt,"a",@progbits
  .balign 4
  .word \block
  .word \annot
  .hword (\count) | ((\waypoint) << 15)
  .set .Lnsends, 0
  .irp s, \sends
  .ifnb \s
  .set .Lnsends, .Lnsends + 1
  .endif
  .endr
  .hword .Lnsends
  .irp s, \sends
  .ifnb \s
  .hword \s
  .endif
  .endr
  .balign 4
  .popsection
.endm

.macro skip start, end
  .pushsection .hbngskip,"",@progbits
  .word \start
  .word \end
  .popsection
.endm

.text
.globl _start
.type _start,%function
_start:
  mov x0, #0
  b f
.Lstart_end:
  nop
.type f,%function
f:
  mov x1, #0
  mov x2, #1
  nop
  nop
.Lloop:
  add x1, x1, x2
  cmp x1, #10
  b.lt .Lloop
.type call,%function
call:
  bl g
.type noreturn,%function
noreturn:
  bl stop
  udf #0
.type g,%function
g:
  ret
.type stop,%function
stop:
  b stop

.section .hbngannot,"a",@progbits
annot:
  .ascii "@0\nEND\n"

.ifndef UNCOVERED
.ifdef SKIP
  skip _start, f + 4
.else
  skip _start, .Lstart_end
.endif
.endif
  entry stop, annot, 1, 1
.ifdef ANNOT
  entry g, g, 1, 1
.else
.ifdef WAYPOINT
  entry g, annot, 1, 0
.else
  entry g, annot, 1, 1
.endif
.endif
  entry noreturn, annot, 1, 1
.ifndef CONTINUE
  entry call, annot, 1, 1, 0
.endif
.ifdef OVERLAP
  entry .Lloop, annot, 4, 0
.else
  entry .Lloop, annot, 3, 1
.endif
.ifdef WAYPOINT
  entry f, annot, 2, 1
.else
  entry f, annot, 2, 0
.endif
.ifdef OUTSIDE
  entry annot, annot, 1, 0
.endif
