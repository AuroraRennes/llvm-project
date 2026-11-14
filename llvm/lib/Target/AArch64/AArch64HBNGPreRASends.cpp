//===- AArch64HBNGPreRASends.cpp - HBNG sends before register allocation --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prototype (-hbng-prera-sends): the sends are decided on machine SSA, before
// register allocation.
//
// The channel has a fixed address, the symbol __hbng_chan (page aligned), so
// a send is "adrp xT, __hbng_chan" + "str xA, [xT]" and needs no reserved
// register: the page is a virtual register like any other, which the
// allocator rematerializes next to the send under pressure (an adrp of a
// symbol is as cheap as a move) rather than spilling it.
//
// Two passes:
//
// - AArch64HBNGPreRASends, at the end of addPreRegAlloc: before every access
//   the annotation pass will describe (after a store-exclusive), one
//   HBNG_SEND per address register its annotation refers to, through the
//   page defined once at the function's entry.
//
// - AArch64HBNGCheckSends, after register allocation, the post-RA load/store
//   optimizer, scheduling and the prologue and epilogue: for every access the
//   annotations describe, each register its annotation refers to must pair
//   with a send: the nearest preceding send of that physical register in the
//   same block, not written between the send and the access. Where not, it
//   inserts a fallback send, through a free register loaded with the page
//   (an adrp).
//
// The sends stay pseudos until emission (AArch64AsmPrinter), so the scheduler
// and the load/store optimizer leave them in place (side effects) and the
// annotation pass recognises them by opcode (isHBNGSend).
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64HBNGMemAccess.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "hbng-prera-sends"

// The channel symbol the sends store to, page aligned (the store has no
// :lo12: offset).
static constexpr const char *HBNGChannelSymbol = "__hbng_chan";

// Before register allocation.
STATISTIC(NumPreRASends, "Sends inserted before register allocation");
STATISTIC(NumPreRAPages, "Channel page adrps inserted before register "
                         "allocation");
// Fallbacks: one event per access, registers apart.
STATISTIC(NumFallbacks, "Fallback events (accesses sent after RA)");
STATISTIC(NumFallbackRegs, "Registers sent by fallbacks");
// The final code.
STATISTIC(NumFinalSendInstrs, "Send instructions emitted");
STATISTIC(NumFinalSentRegs, "Registers sent in the final code");
STATISTIC(NumFinalPages, "Channel page adrps in the final code");

namespace {

// Whether MI is an "adrp <reg>, __hbng_chan".
bool isChannelPage(const MachineInstr &MI) {
  if (MI.getOpcode() != AArch64::ADRP || !MI.getOperand(1).isSymbol())
    return false;
  return StringRef(MI.getOperand(1).getSymbolName()) == HBNGChannelSymbol;
}

// The annotation pass ends a block (for the trace) after a branch, return or
// call that is not the block's last instruction, and after an svc in inline
// asm: no value pairs with a send across it.
bool endsAnnotationBlock(const MachineInstr &MI) {
  return MI.isBranch() || MI.isReturn() || MI.isCall() ||
         (MI.isInlineAsm() &&
          StringRef(MI.getOperand(0).getSymbolName()).contains("svc"));
}

//===----------------------------------------------------------------------===//
// Before register allocation

class AArch64HBNGPreRASends : public MachineFunctionPass {
  const AArch64InstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  Register FunctionPage;

  Register getPage(MachineFunction &MF);
  Register getXReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator Where,
                   const DebugLoc &DL, Register W);

public:
  static char ID;
  AArch64HBNGPreRASends() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return "AArch64 HBNG sends before register allocation";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char AArch64HBNGPreRASends::ID = 0;

// The virtual register holding the channel page, defined once at the
// function's entry.
Register AArch64HBNGPreRASends::getPage(MachineFunction &MF) {
  if (!FunctionPage) {
    MachineBasicBlock &Entry = MF.front();
    FunctionPage = MRI->createVirtualRegister(&AArch64::GPR64commonRegClass);
    BuildMI(Entry, Entry.SkipPHIsLabelsAndDebug(Entry.begin()), DebugLoc(),
            TII->get(AArch64::ADRP), FunctionPage)
        .addExternalSymbol(HBNGChannelSymbol, AArch64II::MO_PAGE);
    ++NumPreRAPages;
  }
  return FunctionPage;
}

// The X register to send for a W register offset (as in [x0, w1, sxtw #3]):
// a send stores 64 bits, and the annotation names X1 and reads its low half.
Register AArch64HBNGPreRASends::getXReg(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator Where,
                                        const DebugLoc &DL, Register W) {
  if (W.isPhysical())
    return MRI->getTargetRegisterInfo()->getMatchingSuperReg(
        W, AArch64::sub_32, &AArch64::GPR64RegClass);
  Register Undef = MRI->createVirtualRegister(&AArch64::GPR64RegClass);
  Register X = MRI->createVirtualRegister(&AArch64::GPR64RegClass);
  // A 64-bit register with no defined value: its high half, which the
  // annotation never reads.
  BuildMI(MBB, Where, DL, TII->get(TargetOpcode::IMPLICIT_DEF), Undef);
  // X: that register with W as its low half. The allocator gives X and W the
  // same physical register (w1 in x1): no instruction is emitted.
  BuildMI(MBB, Where, DL, TII->get(TargetOpcode::INSERT_SUBREG), X)
      .addReg(Undef)
      .addReg(W)
      .addImm(AArch64::sub_32);
  return X;
}

bool AArch64HBNGPreRASends::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getFunction().hasFnAttribute("hbng_no_instr"))
    return false;
  TII = MF.getSubtarget<AArch64Subtarget>().getInstrInfo();
  MRI = &MF.getRegInfo();
  FunctionPage = Register();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI, /*PreRA=*/true);
      if (!A)
        continue;
      MachineBasicBlock::iterator Where =
          A->Status >= 0 ? std::next(MI.getIterator()) : MI.getIterator();
      const DebugLoc &DL = MI.getDebugLoc();

      // The base, unless a frame index (it becomes SP or FP, tracked) or a
      // tracked register; the register offset, a W one as its X register.
      SmallVector<Register, 2> AddrRegs;
      const MachineOperand &BaseMO = MI.getOperand(A->Base);
      if (BaseMO.isReg() &&
          (BaseMO.getReg().isVirtual() || !isHBNGTrackedReg(BaseMO.getReg())))
        AddrRegs.push_back(BaseMO.getReg());
      if (A->isRegOffset()) {
        Register Off = MI.getOperand(A->Offset).getReg();
        if (A->Mode == HBNGMemAccess::RegOffsetW)
          Off = getXReg(MBB, MI.getIterator(), DL, Off);
        AddrRegs.push_back(Off);
      }
      if (AddrRegs.empty())
        continue;

      Register Page = getPage(MF);
      for (unsigned I = 0; I < A->NumData; ++I)
        for (Register R : AddrRegs) {
          BuildMI(MBB, Where, DL, TII->get(AArch64::HBNG_SEND))
              .addReg(Page)
              .addReg(R);
          ++NumPreRASends;
        }
      Changed = true;
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// After register allocation

// The registers a fallback may take for the page, in order of preference:
// the temporaries, the intra-procedure-call ones, then the others (never FP,
// which the annotations track, nor SP).
const MCPhysReg PageRegOrder[] = {
    AArch64::X9,  AArch64::X10, AArch64::X11, AArch64::X12, AArch64::X13,
    AArch64::X14, AArch64::X15, AArch64::X16, AArch64::X17, AArch64::X8,
    AArch64::X0,  AArch64::X1,  AArch64::X2,  AArch64::X3,  AArch64::X4,
    AArch64::X5,  AArch64::X6,  AArch64::X7,  AArch64::X18, AArch64::X19,
    AArch64::X20, AArch64::X21, AArch64::X22, AArch64::X23, AArch64::X24,
    AArch64::X25, AArch64::X26, AArch64::X27, AArch64::X28, AArch64::LR};

class AArch64HBNGCheckSends : public MachineFunctionPass {
  const AArch64InstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  // Per block, in program order (the pairing, see runOnMachineFunction), for
  // each register, its nearest preceding send, if no instruction wrote the
  // register since.
  DenseMap<Register, MachineInstr *> Avail;
  SmallPtrSet<MachineInstr *, 16> Inserted; // by fallbacks: not checked

  SmallVector<Register, 4> addressSends(const MachineInstr &MI,
                                        const HBNGMemAccess &A) const;
  void noteWrites(const MachineInstr &MI);
  void noteSend(MachineInstr &Send);
  void checkBefore(MachineInstr &MI, ArrayRef<Register> Regs);
  void insertFallback(MachineBasicBlock &MBB, MachineBasicBlock::iterator Where,
                      const MachineInstr &MI, ArrayRef<Register> Regs);

public:
  static char ID;
  AArch64HBNGCheckSends() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return "AArch64 HBNG check sends after register allocation";
  }
};

char AArch64HBNGCheckSends::ID = 0;

// The registers the annotation of MI refers to, one send each per data
// register.
SmallVector<Register, 4>
AArch64HBNGCheckSends::addressSends(const MachineInstr &MI,
                                    const HBNGMemAccess &A) const {
  SmallVector<Register, 2> AddrRegs;
  Register Base = MI.getOperand(A.Base).getReg();
  if (!isHBNGTrackedReg(Base))
    AddrRegs.push_back(Base);
  if (A.isRegOffset()) {
    Register Off = MI.getOperand(A.Offset).getReg();
    if (A.Mode == HBNGMemAccess::RegOffsetW)
      Off = TRI->getMatchingSuperReg(Off, AArch64::sub_32,
                                     &AArch64::GPR64RegClass);
    AddrRegs.push_back(Off);
  }
  SmallVector<Register, 4> Sends;
  for (unsigned I = 0; I < A.NumData; ++I)
    Sends.append(AddrRegs.begin(), AddrRegs.end());
  return Sends;
}

// The sends of the registers MI writes no longer pair with a later value.
void AArch64HBNGCheckSends::noteWrites(const MachineInstr &MI) {
  for (auto &[R, S] : Avail)
    if (S && R != AArch64::XZR && MI.modifiesRegister(R, TRI))
      S = nullptr;
}

void AArch64HBNGCheckSends::noteSend(MachineInstr &Send) {
  for (Register R : getHBNGSendPseudoRegs(Send))
    Avail[R] = &Send;
}

// MI's values pair with the nearest preceding send of each register, which
// no instruction in between writes; a fallback sends the missing ones right
// before MI.
void AArch64HBNGCheckSends::checkBefore(MachineInstr &MI,
                                        ArrayRef<Register> Regs) {
  if (Regs.empty())
    return;
  SmallVector<Register, 4> Missing;
  for (Register R : Regs)
    if (!Avail.lookup(R))
      Missing.push_back(R);
  if (!Missing.empty())
    insertFallback(*MI.getParent(), MI.getIterator(), MI, Missing);
}

// Sends Regs at Where through a free register loaded with the page, and
// notes them as the nearest sends of their registers.
void AArch64HBNGCheckSends::insertFallback(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Where,
                                           const MachineInstr &MI,
                                           ArrayRef<Register> Regs) {
  const DebugLoc &DL = MI.getDebugLoc();
  ++NumFallbacks;
  NumFallbackRegs += Regs.size();

  // A register is never chosen for the page if the annotations track it (FP)
  // or it is reserved (SP, or a register reserved with -ffixed-xN).
  auto Candidate = [&](MCPhysReg R) {
    return R != AArch64::FP && !MRI->isReserved(R) &&
           !llvm::is_contained(Regs, R);
  };

  Register Page;
  LivePhysRegs Live(*TRI);
  Live.addLiveOuts(MBB);
  for (auto I = MBB.end(); I != Where;)
    Live.stepBackward(*--I);
  for (MCPhysReg R : PageRegOrder)
    if (Candidate(R) && Live.available(*MRI, R)) {
      Page = R;
      break;
    }
  if (!Page)
    reportFatalInternalError("HBNG: no free register for the channel page");
  Inserted.insert(BuildMI(MBB, Where, DL, TII->get(AArch64::ADRP), Page)
                      .addExternalSymbol(HBNGChannelSymbol,
                                         AArch64II::MO_PAGE));
  // The adrp writes the register: its earlier send no longer pairs.
  if (MachineInstr *&S = Avail[Page])
    S = nullptr;

  for (Register R : Regs) {
    MachineInstr *S = BuildMI(MBB, Where, DL, TII->get(AArch64::HBNG_SEND))
                          .addReg(Page)
                          .addReg(R);
    Inserted.insert(S);
    Avail[R] = S;
  }
}

// Every address register an annotation refers to (<Xn>) must pair with a
// send: the nearest preceding send of Xn in the block (the annotation pass's:
// a machine block, also ended after a call or an svc) that no instruction in
// between writes. The pass checks each in program order and inserts the
// missing sends (fallbacks).
bool AArch64HBNGCheckSends::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getFunction().hasFnAttribute("hbng_no_instr"))
    return false;
  const AArch64Subtarget &STI = MF.getSubtarget<AArch64Subtarget>();
  TII = STI.getInstrInfo();
  TRI = STI.getRegisterInfo();
  MRI = &MF.getRegInfo();
  if (!MRI->tracksLiveness())
    reportFatalUsageError("HBNG: the send check needs liveness, in function '" +
                          MF.getName() + "'");

  for (MachineBasicBlock &MBB : MF) {
    Avail.clear();
    Inserted.clear();
    // What a fallback inserts after MI may or may not be visited: it is
    // skipped, insertFallback notes it.
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      if (Inserted.count(&MI))
        continue;
      assert((!MI.isBundle() ||
              llvm::none_of(make_range(std::next(MI.getIterator()),
                                       getBundleEnd(MI.getIterator())),
                            [](const MachineInstr &BI) {
                              return getHBNGMemAccess(BI).has_value();
                            })) &&
             "HBNG: load or store inside a bundle");

      if (isHBNGSendPseudo(MI)) {
        noteSend(MI);
        continue;
      }

      // A store-exclusive's sends follow it: not checked.
      if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI);
          A && A->Status < 0)
        checkBefore(MI, addressSends(MI, *A));

      noteWrites(MI);

      if (endsAnnotationBlock(MI))
        Avail.clear();
    }

    for (MachineInstr &MI : MBB) {
      if (isHBNGSendPseudo(MI)) {
        ++NumFinalSendInstrs;
        NumFinalSentRegs += getHBNGSendPseudoRegs(MI).size();
      } else if (isChannelPage(MI)) {
        ++NumFinalPages;
      }
    }
  }
  return true;
}

} // end anonymous namespace

INITIALIZE_PASS(AArch64HBNGPreRASends, "hbng-prera-sends-insert",
                "HardBlare-NG sends before register allocation", false, false)
INITIALIZE_PASS(AArch64HBNGCheckSends, "hbng-check-sends",
                "HardBlare-NG check sends after register allocation", false,
                false)

FunctionPass *llvm::createAArch64HBNGPreRASendsPass() {
  return new AArch64HBNGPreRASends();
}
FunctionPass *llvm::createAArch64HBNGCheckSendsPass() {
  return new AArch64HBNGCheckSends();
}
