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
// AArch64HBNGPreRASends, at the end of addPreRegAlloc: before every access
// the annotation pass will describe (after a store-exclusive), one HBNG_SEND
// per address register its annotation refers to, through the page defined
// once at the function's entry.
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
#include "llvm/ADT/Statistic.h"
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

STATISTIC(NumPreRASends, "Sends inserted before register allocation");
STATISTIC(NumPreRAPages, "Channel page adrps inserted before register "
                         "allocation");

namespace {

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

} // end anonymous namespace

INITIALIZE_PASS(AArch64HBNGPreRASends, "hbng-prera-sends-insert",
                "HardBlare-NG sends before register allocation", false, false)

FunctionPass *llvm::createAArch64HBNGPreRASendsPass() {
  return new AArch64HBNGPreRASends();
}
