//===- llvm/Support/HBNGFormat.h - HBNG block table format ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The layout of the HBNG block table (.hbngbbt), its index (.hbngidx), the
// untracked ranges (.hbngskip) and the format version (.hbnginfo), shared by
// the AArch64 AsmPrinter, which emits the table, and llvm-hbng-postlink, which
// sorts, indexes and checks it after the link.
//
// A table entry, little-endian, 4-aligned:
//   uint32_t BlockAddr;   the block's first instruction
//   uint32_t AnnotAddr;   the block's annotations in .hbngannot
//   uint16_t Count;       instruction count; the top bit is set when the
//                         block's last instruction is a waypoint (a branch,
//                         call, return or isb)
//   uint16_t NumSends;
//   uint16_t SendIndex[NumSends];  the instruction of each send, in channel
//                                  order (twice for an stp)
//   padding to 4 bytes
//
// An index record: uint32_t BlockAddr; uint32_t EntryOffset (from the start of
// the sorted .hbngbbt). Records are sorted by address.
//
// A skip range: uint32_t Start; uint32_t End (exclusive).
//
// .hbnginfo: uint16_t Version.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_HBNGFORMAT_H
#define LLVM_SUPPORT_HBNGFORMAT_H

#include <cstddef>
#include <cstdint>

namespace llvm {
namespace hbng {

/// Bumped with any change to the entry, the index or the annotations.
constexpr uint16_t FormatVersion = 1;

constexpr const char *TableSection = ".hbngbbt";
constexpr const char *AnnotSection = ".hbngannot";
constexpr const char *IndexSection = ".hbngidx";
constexpr const char *SkipSection = ".hbngskip";
constexpr const char *InfoSection = ".hbnginfo";

constexpr unsigned EntryAlign = 4;
/// The fixed part of an entry: two addresses, the count and the send count.
constexpr unsigned EntryHeaderSize = 12;
constexpr unsigned SendIndexSize = 2;
constexpr uint16_t WaypointBit = 0x8000;
constexpr unsigned MaxBlockInstrs = 0x7fff;

constexpr unsigned IndexRecordSize = 8;
constexpr unsigned SkipRecordSize = 8;
constexpr unsigned InfoSize = 2;

/// The size of an entry with NumSends sends, padding included.
constexpr size_t entrySize(unsigned NumSends) {
  return (EntryHeaderSize + SendIndexSize * NumSends + EntryAlign - 1) &
         ~size_t(EntryAlign - 1);
}

} // namespace hbng
} // namespace llvm

#endif // LLVM_SUPPORT_HBNGFORMAT_H
