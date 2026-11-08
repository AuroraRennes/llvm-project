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
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include <cassert>
#include <optional>

namespace llvm {

// The register holding the STM stimulus address the sends store to. The
// program is built with x18 reserved (-ffixed-x18, checked by the send pass),
// so no other instruction uses it as a base.
constexpr unsigned HBNGSendReg = AArch64::X18;

// The registers whose values the annotations track: SP and FP (x29). Their
// first values are sent once by the start code. From then on:
//  - an update by an immediate from SP or FP (and the write-back of an access
//    based on them) is computed from the annotation, with no send;
//  - any other write of SP is an ALU instruction (SP cannot be loaded, nor
//    stored to the STM): its untracked source registers are sent before it,
//    like address registers, and the annotation computes the value;
//  - any other write of FP (the epilogue's reload, a move) sends the new
//    value right after the instruction, the only point where it exists.
// An access based on SP or FP sends nothing.
inline bool isHBNGTrackedReg(Register R) {
  return R == AArch64::SP || R == AArch64::FP;
}

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

  // NEON structured accesses (LD1-LD4, ST1-ST4), one annotation line and one
  // send per data register, in register order:
  //  - VecMultiple: ld1/st1 of 1-4 registers, register r at byte r * RegBytes;
  //  - VecInterleaved: ld2-ld4/st2-st4, element i of register r at byte
  //    (i * NumData + r) * ElemSize;
  //  - VecReplicate: ld1r-ld4r, one element per register, at r * ElemSize,
  //    replicated to every element of the register;
  //  - VecLane: one lane per register, at r * ElemSize; the other lanes are
  //    kept.
  enum VecKind { NotVec, VecMultiple, VecInterleaved, VecReplicate, VecLane };

  bool IsStore;
  unsigned Size;     // bytes moved per data register
  ModeKind Mode;
  unsigned NumData;  // 1, or 2 for pairs (1-4 structured registers)
  unsigned FirstData; // operand index of the first data register (or of the
                      // register list of a structured access)
  unsigned Base;     // operand index of the base register
  int Offset;        // operand index of the offset (imm or register), or -1;
                     // for a structured post-index access, the increment
                     // register (XZR: by the number of bytes accessed)
  int Status;        // operand index of the store-exclusive status, or -1
  VecKind Vec = NotVec;
  unsigned ElemSize = 0; // structured: bytes per element
  unsigned RegBytes = 0; // structured: 8 or 16, the width of each register
  int Lane = -1;         // VecLane: operand index of the lane number

  bool hasWriteback() const { return Mode == PreIndex || Mode == PostIndex; }
  bool isRegOffset() const { return Mode == RegOffsetW || Mode == RegOffsetX; }
  // Whether the immediate is in units of Size (scaled) rather than bytes.
  bool isScaledImm() const {
    return Vec == NotVec &&
           (Mode == ScaledImm || (hasWriteback() && NumData == 2));
  }
};

// The structured access MI performs, std::nullopt if it is not one.
// Operands, in order: the written-back base (post-index), the register list
// (loads: its def, then for a lane form the tied use), the lane number (lane
// forms), the base, the increment register (post-index).
inline std::optional<HBNGMemAccess> getHBNGVecAccess(const MachineInstr &MI) {
  using M = HBNGMemAccess;
  bool IsStore = false, Post = false;
  M::VecKind Kind = M::NotVec;
  unsigned NumRegs = 0, Elem = 0, RegBytes = 0;

#define HBNG_VEC(OPC, ST, KIND, NREG, ESZ, RB)                                 \
  case AArch64::OPC:                                                           \
  case AArch64::OPC##_POST:                                                    \
    IsStore = ST; Kind = M::KIND; NumRegs = NREG; Elem = ESZ; RegBytes = RB;   \
    Post = MI.getOpcode() == AArch64::OPC##_POST;                              \
    break;
// The arrangements of a multiple-register access (v1d: ld1, st1, ldNr only).
#define HBNG_VEC_ARR(P, ST, KIND, NREG)                                        \
  HBNG_VEC(P##v16b, ST, KIND, NREG, 1, 16) HBNG_VEC(P##v8b, ST, KIND, NREG, 1, 8) \
  HBNG_VEC(P##v8h, ST, KIND, NREG, 2, 16) HBNG_VEC(P##v4h, ST, KIND, NREG, 2, 8)  \
  HBNG_VEC(P##v4s, ST, KIND, NREG, 4, 16) HBNG_VEC(P##v2s, ST, KIND, NREG, 4, 8)  \
  HBNG_VEC(P##v2d, ST, KIND, NREG, 8, 16)
#define HBNG_VEC_ARR1D(P, ST, KIND, NREG)                                      \
  HBNG_VEC_ARR(P, ST, KIND, NREG) HBNG_VEC(P##v1d, ST, KIND, NREG, 8, 8)
#define HBNG_VEC_LANE(P, ST, NREG)                                             \
  HBNG_VEC(P##i8, ST, VecLane, NREG, 1, 16)                                    \
  HBNG_VEC(P##i16, ST, VecLane, NREG, 2, 16)                                   \
  HBNG_VEC(P##i32, ST, VecLane, NREG, 4, 16)                                   \
  HBNG_VEC(P##i64, ST, VecLane, NREG, 8, 16)

  switch (MI.getOpcode()) {
  default:
    return std::nullopt;
  HBNG_VEC_ARR1D(LD1One, false, VecMultiple, 1)
  HBNG_VEC_ARR1D(LD1Two, false, VecMultiple, 2)
  HBNG_VEC_ARR1D(LD1Three, false, VecMultiple, 3)
  HBNG_VEC_ARR1D(LD1Four, false, VecMultiple, 4)
  HBNG_VEC_ARR(LD2Two, false, VecInterleaved, 2)
  HBNG_VEC_ARR(LD3Three, false, VecInterleaved, 3)
  HBNG_VEC_ARR(LD4Four, false, VecInterleaved, 4)
  HBNG_VEC_ARR1D(LD1R, false, VecReplicate, 1)
  HBNG_VEC_ARR1D(LD2R, false, VecReplicate, 2)
  HBNG_VEC_ARR1D(LD3R, false, VecReplicate, 3)
  HBNG_VEC_ARR1D(LD4R, false, VecReplicate, 4)
  HBNG_VEC_LANE(LD1, false, 1) HBNG_VEC_LANE(LD2, false, 2)
  HBNG_VEC_LANE(LD3, false, 3) HBNG_VEC_LANE(LD4, false, 4)
  HBNG_VEC_ARR1D(ST1One, true, VecMultiple, 1)
  HBNG_VEC_ARR1D(ST1Two, true, VecMultiple, 2)
  HBNG_VEC_ARR1D(ST1Three, true, VecMultiple, 3)
  HBNG_VEC_ARR1D(ST1Four, true, VecMultiple, 4)
  HBNG_VEC_ARR(ST2Two, true, VecInterleaved, 2)
  HBNG_VEC_ARR(ST3Three, true, VecInterleaved, 3)
  HBNG_VEC_ARR(ST4Four, true, VecInterleaved, 4)
  HBNG_VEC_LANE(ST1, true, 1) HBNG_VEC_LANE(ST2, true, 2)
  HBNG_VEC_LANE(ST3, true, 3) HBNG_VEC_LANE(ST4, true, 4)
  }
#undef HBNG_VEC_LANE
#undef HBNG_VEC_ARR1D
#undef HBNG_VEC_ARR
#undef HBNG_VEC

  HBNGMemAccess A;
  A.IsStore = IsStore;
  A.Vec = Kind;
  A.ElemSize = Elem;
  A.RegBytes = RegBytes;
  A.NumData = NumRegs;
  A.Size = (Kind == M::VecMultiple || Kind == M::VecInterleaved) ? RegBytes
                                                                 : Elem;
  A.Mode = Post ? M::PostIndex : M::NoOffset;
  A.Status = -1;
  A.FirstData = Post ? 1 : 0;
  unsigned Next = A.FirstData + 1;
  if (Kind == M::VecLane) {
    if (!IsStore)
      ++Next; // the tied register list
    A.Lane = Next++;
  }
  A.Base = Next;
  A.Offset = Post ? int(A.Base) + 1 : -1;

  assert(MI.getOperand(A.Base).isReg() &&
         AArch64::GPR64spRegClass.contains(MI.getOperand(A.Base).getReg()) &&
         "HBNG: base operand is not a 64-bit base register");
  assert((A.Lane < 0 || MI.getOperand(A.Lane).isImm()) &&
         "HBNG: lane number expected");
  assert((A.Offset < 0 || MI.getOperand(A.Offset).isReg()) &&
         "HBNG: increment register expected");
  return A;
}

// Data register I of the access: an operand, or a register of the list of a
// structured access.
inline Register getHBNGDataReg(const MachineInstr &MI, const HBNGMemAccess &A,
                               unsigned I, const TargetRegisterInfo *TRI) {
  if (A.Vec == HBNGMemAccess::NotVec)
    return MI.getOperand(A.FirstData + I).getReg();
  Register List = MI.getOperand(A.FirstData).getReg();
  if (AArch64::FPR128RegClass.contains(List) ||
      AArch64::FPR64RegClass.contains(List)) {
    assert(I == 0 && "HBNG: one register in the list");
    return List;
  }
  static const unsigned QSub[] = {AArch64::qsub0, AArch64::qsub1,
                                  AArch64::qsub2, AArch64::qsub3};
  static const unsigned DSub[] = {AArch64::dsub0, AArch64::dsub1,
                                  AArch64::dsub2, AArch64::dsub3};
  bool IsQ = AArch64::QQRegClass.contains(List) ||
             AArch64::QQQRegClass.contains(List) ||
             AArch64::QQQQRegClass.contains(List);
  Register R = TRI->getSubReg(List, (IsQ ? QSub : DSub)[I]);
  assert(R && "HBNG: register list shorter than the access");
  return R;
}

// The bytes added to the base by the write-back of A, if it is an immediate.
inline std::optional<int64_t> getHBNGWritebackBytes(const MachineInstr &MI,
                                                    const HBNGMemAccess &A) {
  if (!A.hasWriteback())
    return std::nullopt;
  const MachineOperand &Off = MI.getOperand(A.Offset);
  if (A.Vec != HBNGMemAccess::NotVec) {
    if (Off.getReg() != AArch64::XZR)
      return std::nullopt; // by a register
    return int64_t(A.NumData) * A.Size;
  }
  return Off.getImm() * (A.isScaledImm() ? int64_t(A.Size) : 1);
}

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
    return getHBNGVecAccess(MI);
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
  // Non-temporal pairs: scaled immediate only.
  LDP(LDNPWi, 4, ScaledImm) LDP(LDNPXi, 8, ScaledImm) LDP(LDNPSi, 4, ScaledImm)
  LDP(LDNPDi, 8, ScaledImm) LDP(LDNPQi, 16, ScaledImm)
  STP(STNPWi, 4, ScaledImm) STP(STNPXi, 8, ScaledImm) STP(STNPSi, 4, ScaledImm)
  STP(STNPDi, 8, ScaledImm) STP(STNPQi, 16, ScaledImm)
  // Acquire loads and release stores with an unscaled offset (RCPC2, RCPC3).
  LD(LDAPURBi, 1, UnscaledImm) LD(LDAPURSBWi, 1, UnscaledImm)
  LD(LDAPURSBXi, 1, UnscaledImm) LD(LDAPURHi, 2, UnscaledImm)
  LD(LDAPURSHWi, 2, UnscaledImm) LD(LDAPURSHXi, 2, UnscaledImm)
  LD(LDAPURi, 4, UnscaledImm) LD(LDAPURSWi, 4, UnscaledImm)
  LD(LDAPURXi, 8, UnscaledImm) LD(LDAPURbi, 1, UnscaledImm)
  LD(LDAPURhi, 2, UnscaledImm) LD(LDAPURsi, 4, UnscaledImm)
  LD(LDAPURdi, 8, UnscaledImm) LD(LDAPURqi, 16, UnscaledImm)
  ST(STLURBi, 1, UnscaledImm) ST(STLURHi, 2, UnscaledImm)
  ST(STLURWi, 4, UnscaledImm) ST(STLURXi, 8, UnscaledImm)
  ST(STLURbi, 1, UnscaledImm) ST(STLURhi, 2, UnscaledImm)
  ST(STLURsi, 4, UnscaledImm) ST(STLURdi, 8, UnscaledImm)
  ST(STLURqi, 16, UnscaledImm)
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

// An update of a tracked register the annotations compute: Rd <- Rn + Imm
// with Rd and Rn tracked (ADD/SUB immediate, e.g. "mov x29, sp",
// "sub sp, sp, #16", "mov sp, x29").
struct HBNGTrackedUpdate {
  Register Dst;
  Register Src;
  int64_t Imm; // in bytes, negative for SUB
};

inline std::optional<HBNGTrackedUpdate>
getHBNGTrackedImmUpdate(const MachineInstr &MI) {
  if (MI.getOpcode() != AArch64::ADDXri && MI.getOpcode() != AArch64::SUBXri)
    return std::nullopt;
  if (!MI.getOperand(2).isImm())
    return std::nullopt; // a symbol (:lo12:), not a constant
  Register Dst = MI.getOperand(0).getReg(), Src = MI.getOperand(1).getReg();
  if (!isHBNGTrackedReg(Dst) || !isHBNGTrackedReg(Src))
    return std::nullopt;
  int64_t Imm = MI.getOperand(2).getImm()
                << AArch64_AM::getShiftValue(MI.getOperand(3).getImm());
  return HBNGTrackedUpdate{Dst, Src,
                           MI.getOpcode() == AArch64::SUBXri ? -Imm : Imm};
}

// Whether MI writes R (or a part of it) through a def operand. A call
// preserves SP and FP (AAPCS64), although it carries an implicit def of SP and
// its register mask does not list SP.
inline bool hbngDefinesReg(const MachineInstr &MI, Register R,
                           const TargetRegisterInfo *TRI) {
  if (MI.isCall())
    return false;
  return llvm::any_of(MI.operands(), [&](const MachineOperand &MO) {
    return MO.isReg() && MO.isDef() && MO.getReg().isPhysical() &&
           TRI->regsOverlap(MO.getReg(), R);
  });
}

// Whether the write of the tracked register R by MI is computed from the
// annotation with no send: an immediate update from a tracked register, or
// the write-back of an access based on R (base + immediate).
inline bool isHBNGComputedTrackedWrite(const MachineInstr &MI, Register R,
                                       const TargetRegisterInfo *TRI) {
  if (std::optional<HBNGTrackedUpdate> U = getHBNGTrackedImmUpdate(MI))
    if (U->Dst == R)
      return true;
  std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI);
  return A && MI.getOperand(A->Base).getReg() == R &&
         getHBNGWritebackBytes(MI, *A) &&
         llvm::none_of(llvm::seq(0u, A->NumData), [&](unsigned I) {
           return TRI->regsOverlap(getHBNGDataReg(MI, *A, I, TRI), R);
         });
}

// A write of SP that is not computed from tracked registers only. Supported
// is false for an opcode whose value the annotations do not describe (none
// is known: these are all the instructions that can write SP).
struct HBNGSPWrite {
  bool Supported;
  bool IsSub;              // ADD/SUB: subtracts the second operand
  int Rn;                  // operand index of the first source
  int Rm;                  // operand index of the register source, or -1
  SmallVector<Register, 2> Sends; // untracked sources (X registers), sent
                                  // before MI
};

inline std::optional<HBNGSPWrite>
getHBNGSPWrite(const MachineInstr &MI, const TargetRegisterInfo *TRI) {
  if (!hbngDefinesReg(MI, AArch64::SP, TRI) ||
      isHBNGComputedTrackedWrite(MI, AArch64::SP, TRI))
    return std::nullopt;
  HBNGSPWrite W{false, false, -1, -1, {}};
  switch (MI.getOpcode()) {
  case AArch64::SUBXri:
    W.IsSub = true;
    [[fallthrough]];
  case AArch64::ADDXri:
  case AArch64::ANDXri:
  case AArch64::ORRXri:
  case AArch64::EORXri:
    W.Supported = MI.getOperand(2).isImm();
    W.Rn = 1;
    break;
  case AArch64::SUBXrx:
  case AArch64::SUBXrx64:
    W.IsSub = true;
    [[fallthrough]];
  case AArch64::ADDXrx:
  case AArch64::ADDXrx64:
    W.Supported = true;
    W.Rn = 1;
    W.Rm = 2;
    break;
  default:
    return W;
  }
  for (int I : {W.Rn, W.Rm}) {
    if (I < 0)
      continue;
    Register R = MI.getOperand(I).getReg();
    if (AArch64::GPR32allRegClass.contains(R))
      R = TRI->getMatchingSuperReg(R, AArch64::sub_32,
                                   &AArch64::GPR64allRegClass);
    if (!isHBNGTrackedReg(R) && R != AArch64::XZR)
      W.Sends.push_back(R);
  }
  return W;
}

// Whether MI writes FP with a value the annotations cannot compute: its new
// value is sent right after MI.
inline bool isHBNGFPSentAfter(const MachineInstr &MI,
                              const TargetRegisterInfo *TRI) {
  return hbngDefinesReg(MI, AArch64::FP, TRI) &&
         !isHBNGComputedTrackedWrite(MI, AArch64::FP, TRI);
}

// Whether MI is a send inserted by the STM send pass (a store based on the
// reserved register). The annotation pass runs after it and skips them.
inline bool isHBNGSend(const MachineInstr &MI) {
  std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI);
  return A && A->IsStore && MI.getOperand(A->Base).getReg() == HBNGSendReg;
}

} // end namespace llvm

#endif
