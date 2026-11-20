# REQUIRES: aarch64-registered-target, ld.lld
## llvm-hbng-postlink sorts the block table by address, and adds its index and
## the format version.

# RUN: echo 'SECTIONS { . = 0x10000; .text : { *(.text) } . = 0x20000; \
# RUN:   .hbngannot : { *(.hbngannot) } .hbngbbt : { *(.hbngbbt) } }' > %t.lds

## A table written out of order.
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

  skip _start, .Lstart_end
  entry stop, annot, 1, 1
  entry g, annot, 1, 1
  entry noreturn, annot, 1, 1
  entry call, annot, 1, 1, 0
  entry .Lloop, annot, 3, 1
  entry f, annot, 2, 0
