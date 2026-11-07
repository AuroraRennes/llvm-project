//===- AArch64HBNGMemAccess.h - HBNG description of loads and stores ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One description of the loads and stores supported by HBNG, shared by the
// annotation pass (which describes the access) and the STM send pass (which
// sends the address registers the annotation refers to), so that the two
// cannot disagree on which instructions are covered or where their operands
// are.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64HBNGMEMACCESS_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64HBNGMEMACCESS_H

#include "AArch64InstrInfo.h"
#include "AArch64RegisterInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include <cassert>
#include <optional>

namespace llvm {

struct HBNGMemAccess {
  enum ModeKind {
    ScaledImm,   // [Rn, #imm * Size]
    UnscaledImm, // [Rn, #imm]
    PreIndex,    // [Rn, #imm]!  (imm scaled by Size for pairs only)
    PostIndex,   // [Rn], #imm   (imm scaled by Size for pairs only)
    RegOffsetW,  // [Rn, Wm, {s,u}xtw {#log2(Size)}]
    RegOffsetX,  // [Rn, Xm, {lsl,sxtx} {#log2(Size)}]
    NoOffset     // [Rn]  (acquire/release and exclusives)
  };

  bool IsStore;
  unsigned Size;     // bytes moved per data register
  ModeKind Mode;
  unsigned NumData;  // 1, or 2 for pairs
  unsigned FirstData; // operand index of the first data register
  unsigned Base;     // operand index of the base register
  int Offset;        // operand index of the offset (imm or register), or -1
  int Status;        // operand index of the store-exclusive status, or -1

  bool hasWriteback() const { return Mode == PreIndex || Mode == PostIndex; }
  bool isRegOffset() const { return Mode == RegOffsetW || Mode == RegOffsetX; }
  // Whether the immediate is in units of Size (scaled) rather than bytes.
  bool isScaledImm() const {
    return Mode == ScaledImm || (hasWriteback() && NumData == 2);
  }
};

// The description of MI if HBNG supports it, std::nullopt otherwise.
inline std::optional<HBNGMemAccess> getHBNGMemAccess(const MachineInstr &MI) {
  using M = HBNGMemAccess;
  bool IsStore = false;
  unsigned Size = 0;
  M::ModeKind Mode = M::ScaledImm;
  unsigned NumData = 1;
  bool ExclusiveStore = false;

#define HBNG_ACCESS(OPC, ST, SZ, MODE, N, EXCL)                                \
  case AArch64::OPC:                                                           \
    IsStore = ST; Size = SZ; Mode = M::MODE; NumData = N;                      \
    ExclusiveStore = EXCL;                                                     \
    break;
#define LD(OPC, SZ, MODE) HBNG_ACCESS(OPC, false, SZ, MODE, 1, false)
#define ST(OPC, SZ, MODE) HBNG_ACCESS(OPC, true, SZ, MODE, 1, false)
#define LDP(OPC, SZ, MODE) HBNG_ACCESS(OPC, false, SZ, MODE, 2, false)
#define STP(OPC, SZ, MODE) HBNG_ACCESS(OPC, true, SZ, MODE, 2, false)
// Every addressing mode of one load or store: scaled, unscaled, pre, post,
// W and X register offset.
#define LD_ALL(R, U, SZ)                                                       \
  LD(R##ui, SZ, ScaledImm) LD(U##i, SZ, UnscaledImm) LD(R##pre, SZ, PreIndex)  \
  LD(R##post, SZ, PostIndex) LD(R##roW, SZ, RegOffsetW)                        \
  LD(R##roX, SZ, RegOffsetX)
#define ST_ALL(R, U, SZ)                                                       \
  ST(R##ui, SZ, ScaledImm) ST(U##i, SZ, UnscaledImm) ST(R##pre, SZ, PreIndex)  \
  ST(R##post, SZ, PostIndex) ST(R##roW, SZ, RegOffsetW)                        \
  ST(R##roX, SZ, RegOffsetX)
#define LDP_ALL(P, SZ)                                                         \
  LDP(P##i, SZ, ScaledImm) LDP(P##pre, SZ, PreIndex) LDP(P##post, SZ, PostIndex)
#define STP_ALL(P, SZ)                                                         \
  STP(P##i, SZ, ScaledImm) STP(P##pre, SZ, PreIndex) STP(P##post, SZ, PostIndex)

  switch (MI.getOpcode()) {
  default:
    return std::nullopt;
  // Loads: integer, sign-extending, FP/SIMD.
  LD_ALL(LDRBB, LDURBB, 1) LD_ALL(LDRSBW, LDURSBW, 1) LD_ALL(LDRSBX, LDURSBX, 1)
  LD_ALL(LDRHH, LDURHH, 2) LD_ALL(LDRSHW, LDURSHW, 2) LD_ALL(LDRSHX, LDURSHX, 2)
  LD_ALL(LDRW, LDURW, 4) LD_ALL(LDRSW, LDURSW, 4) LD_ALL(LDRX, LDURX, 8)
  LD_ALL(LDRB, LDURB, 1) LD_ALL(LDRH, LDURH, 2) LD_ALL(LDRS, LDURS, 4)
  LD_ALL(LDRD, LDURD, 8) LD_ALL(LDRQ, LDURQ, 16)
  // Stores.
  ST_ALL(STRBB, STURBB, 1) ST_ALL(STRHH, STURHH, 2) ST_ALL(STRW, STURW, 4)
  ST_ALL(STRX, STURX, 8) ST_ALL(STRB, STURB, 1) ST_ALL(STRH, STURH, 2)
  ST_ALL(STRS, STURS, 4) ST_ALL(STRD, STURD, 8) ST_ALL(STRQ, STURQ, 16)
  // Pairs.
  LDP_ALL(LDPW, 4) LDP_ALL(LDPSW, 4) LDP_ALL(LDPX, 8)
  LDP_ALL(LDPS, 4) LDP_ALL(LDPD, 8) LDP_ALL(LDPQ, 16)
  STP_ALL(STPW, 4) STP_ALL(STPX, 8) STP_ALL(STPS, 4) STP_ALL(STPD, 8)
  STP_ALL(STPQ, 16)
  // Acquire loads, load-exclusive (single and pair).
  LD(LDARB, 1, NoOffset) LD(LDARH, 2, NoOffset) LD(LDARW, 4, NoOffset)
  LD(LDARX, 8, NoOffset) LD(LDAPRB, 1, NoOffset) LD(LDAPRH, 2, NoOffset)
  LD(LDAPRW, 4, NoOffset) LD(LDAPRX, 8, NoOffset) LD(LDLARB, 1, NoOffset)
  LD(LDLARH, 2, NoOffset) LD(LDLARW, 4, NoOffset) LD(LDLARX, 8, NoOffset)
  LD(LDXRB, 1, NoOffset) LD(LDXRH, 2, NoOffset) LD(LDXRW, 4, NoOffset)
  LD(LDXRX, 8, NoOffset) LD(LDAXRB, 1, NoOffset) LD(LDAXRH, 2, NoOffset)
  LD(LDAXRW, 4, NoOffset) LD(LDAXRX, 8, NoOffset)
  LDP(LDXPW, 4, NoOffset) LDP(LDXPX, 8, NoOffset)
  LDP(LDAXPW, 4, NoOffset) LDP(LDAXPX, 8, NoOffset)
  // Release stores.
  ST(STLRB, 1, NoOffset) ST(STLRH, 2, NoOffset) ST(STLRW, 4, NoOffset)
  ST(STLRX, 8, NoOffset) ST(STLLRB, 1, NoOffset) ST(STLLRH, 2, NoOffset)
  ST(STLLRW, 4, NoOffset) ST(STLLRX, 8, NoOffset)
  // Store-exclusive: (Ws status, Rt [, Rt2], Rn).
  HBNG_ACCESS(STXRB, true, 1, NoOffset, 1, true)
  HBNG_ACCESS(STXRH, true, 2, NoOffset, 1, true)
  HBNG_ACCESS(STXRW, true, 4, NoOffset, 1, true)
  HBNG_ACCESS(STXRX, true, 8, NoOffset, 1, true)
  HBNG_ACCESS(STLXRB, true, 1, NoOffset, 1, true)
  HBNG_ACCESS(STLXRH, true, 2, NoOffset, 1, true)
  HBNG_ACCESS(STLXRW, true, 4, NoOffset, 1, true)
  HBNG_ACCESS(STLXRX, true, 8, NoOffset, 1, true)
  HBNG_ACCESS(STXPW, true, 4, NoOffset, 2, true)
  HBNG_ACCESS(STXPX, true, 8, NoOffset, 2, true)
  HBNG_ACCESS(STLXPW, true, 4, NoOffset, 2, true)
  HBNG_ACCESS(STLXPX, true, 8, NoOffset, 2, true)
  }
#undef STP_ALL
#undef LDP_ALL
#undef ST_ALL
#undef LD_ALL
#undef STP
#undef LDP
#undef ST
#undef LD
#undef HBNG_ACCESS

  // Operand layout, in order: the store-exclusive status, the written-back
  // base (pre/post), the data registers, the base, the offset.
  HBNGMemAccess A;
  A.IsStore = IsStore;
  A.Size = Size;
  A.Mode = Mode;
  A.NumData = NumData;
  A.Status = ExclusiveStore ? 0 : -1;
  A.FirstData = (ExclusiveStore ? 1 : 0) + (A.hasWriteback() ? 1 : 0);
  A.Base = A.FirstData + NumData;
  A.Offset = Mode == M::NoOffset ? -1 : int(A.Base) + 1;

  assert(MI.getOperand(A.Base).isReg() &&
         AArch64::GPR64spRegClass.contains(MI.getOperand(A.Base).getReg()) &&
         "HBNG: base operand is not a 64-bit base register");
  assert((!A.isRegOffset() || MI.getOperand(A.Offset).isReg()) &&
         "HBNG: register offset expected");
  assert((A.Offset < 0 || A.isRegOffset() || MI.getOperand(A.Offset).isImm() ||
          MI.getOperand(A.Offset).isGlobal() ||
          MI.getOperand(A.Offset).isCPI() ||
          MI.getOperand(A.Offset).isSymbol() ||
          MI.getOperand(A.Offset).isJTI() ||
          MI.getOperand(A.Offset).isBlockAddress() ||
          MI.getOperand(A.Offset).isMCSymbol()) &&
         "HBNG: immediate offset expected");
  return A;
}

} // end namespace llvm

#endif
