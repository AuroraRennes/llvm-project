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
//   optimizer, scheduling and the prologue and epilogue: for every access
//   (and write of SP or FP) the annotations describe, each register its
//   annotation refers to must pair with a send: the nearest preceding send of
//   that physical register in the same block, not written between the send
//   and the access (for a store-exclusive, the nearest following one; for a
//   new value of FP, the send right after its write). Where not, a send since
//   the previous access that nothing pairs with is given the missing register
//   instead, else it inserts a fallback send. A fallback needs a register for
//   the page: one still holding it from a send of the block, else a free
//   register (an adrp of the page), else a register saved on the stack around
//   the send. Then the sends nothing pairs with are removed (their access is
//   gone or uses other registers), and two adjacent sends through the same
//   page register become one pair (an stp).
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
// After: what became of them.
STATISTIC(NumSendsRemoved, "Pre-RA sends nothing pairs with, removed");
STATISTIC(NumSendsRepaired, "Pre-RA sends nothing paired with, given a "
                            "missing register instead (no fallback)");
STATISTIC(NumFallbacks, "Fallback events (accesses sent after RA)");
STATISTIC(NumFallbackRegs, "Registers sent by fallbacks");
STATISTIC(NumFbPageReused, "Fallbacks through a register still holding the "
                           "page");
STATISTIC(NumFbPageScratch, "Fallbacks through a free register (adrp)");
STATISTIC(NumFbSpills, "Fallbacks through a register saved on the stack");
// The final code.
STATISTIC(NumFinalSendInstrs, "Send instructions (str and stp) emitted");
STATISTIC(NumFinalSendPairs, "Send pairs (stp) emitted");
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
  unsigned sendOpcode(Register R) const;

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

// The send pseudo for R: a W register (a register offset, as in
// [x0, w1, sxtw #3]) is sent itself, stored as its X register, whose low half
// the annotation reads.
unsigned AArch64HBNGPreRASends::sendOpcode(Register R) const {
  const TargetRegisterInfo *TRI = MRI->getTargetRegisterInfo();
  const TargetRegisterClass *RC = R.isVirtual()
                                      ? MRI->getRegClass(R)
                                      : TRI->getMinimalPhysRegClass(R);
  return TRI->getRegSizeInBits(*RC) == 32 ? AArch64::HBNG_SEND_W
                                          : AArch64::HBNG_SEND;
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
      // tracked register; the register offset.
      SmallVector<Register, 2> AddrRegs;
      const MachineOperand &BaseMO = MI.getOperand(A->Base);
      if (BaseMO.isReg() &&
          (BaseMO.getReg().isVirtual() || !isHBNGTrackedReg(BaseMO.getReg())))
        AddrRegs.push_back(BaseMO.getReg());
      if (A->isRegOffset())
        AddrRegs.push_back(MI.getOperand(A->Offset).getReg());
      if (AddrRegs.empty())
        continue;

      // One send per register, even for a pair access: all the values of a
      // register pair with its nearest send.
      Register Page = getPage(MF);
      for (Register R : AddrRegs) {
        BuildMI(MBB, Where, DL, TII->get(sendOpcode(R)))
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
  // The sends some value pairs with (the others are removed).
  SmallPtrSet<MachineInstr *, 16> Used;
  SmallPtrSet<MachineInstr *, 16> Inserted; // by fallbacks: not checked
  // The sends since the previous access that expects some.
  SmallVector<MachineInstr *, 4> Window;
  // A register holding the channel page, from a send or an adrp of the block.
  Register KnownPage;

  SmallVector<Register, 4> addressSends(const MachineInstr &MI,
                                        const HBNGMemAccess &A) const;
  void noteWrites(const MachineInstr &MI);
  void noteSend(MachineInstr &Send);
  bool neededLater(MachineInstr &MI, Register Q) const;
  MachineInstr *repairFromWindow(MachineInstr &MI, Register R);
  void checkBefore(MachineInstr &MI, ArrayRef<Register> Regs);
  void checkAfter(MachineInstr &MI, ArrayRef<Register> Regs);
  MachineBasicBlock::iterator afterOwnSends(MachineInstr &MI);
  void insertFallback(MachineBasicBlock &MBB, MachineBasicBlock::iterator Where,
                      const MachineInstr &MI, ArrayRef<Register> Regs);
  void formPairs(MachineBasicBlock &MBB);

public:
  static char ID;
  AArch64HBNGCheckSends() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return "AArch64 HBNG check sends after register allocation";
  }
};

char AArch64HBNGCheckSends::ID = 0;

// The registers the annotation of MI refers to.
SmallVector<Register, 4>
AArch64HBNGCheckSends::addressSends(const MachineInstr &MI,
                                    const HBNGMemAccess &A) const {
  SmallVector<Register, 4> AddrRegs;
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
  return AddrRegs;
}

// The sends of the registers MI writes no longer pair with a later value.
void AArch64HBNGCheckSends::noteWrites(const MachineInstr &MI) {
  for (auto &[R, S] : Avail)
    if (S && R != AArch64::XZR && MI.modifiesRegister(R, TRI))
      S = nullptr;
  if (KnownPage && MI.modifiesRegister(KnownPage, TRI))
    KnownPage = Register();
}

void AArch64HBNGCheckSends::noteSend(MachineInstr &Send) {
  for (Register R : getHBNGSendPseudoRegs(Send))
    Avail[R] = &Send;
  KnownPage = Send.getOperand(0).getReg();
  Window.push_back(&Send);
}

// Whether an access after MI, in the same annotation block and before any
// write of Q, expects a send of Q.
bool AArch64HBNGCheckSends::neededLater(MachineInstr &MI, Register Q) const {
  for (auto I = std::next(MI.getIterator()); I != MI.getParent()->end(); ++I) {
    if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(*I))
      if (llvm::is_contained(addressSends(*I, *A), Q))
        return true;
    if (I->modifiesRegister(Q, TRI) || endsAnnotationBlock(*I))
      return false;
  }
  return false;
}

// R is missing before MI, but the window may hold a send nothing pairs with
// (the allocator gave its access another register, e.g. the copy the
// two-address pass inserts for the tied base of a write-back access). It is
// given R instead, at no cost: in place if nothing writes R between it and
// MI, else moved right after the last such write (if the page register is
// not written in between).
MachineInstr *AArch64HBNGCheckSends::repairFromWindow(MachineInstr &MI,
                                                      Register R) {
  MachineBasicBlock &MBB = *MI.getParent();
  for (MachineInstr *S : llvm::reverse(Window)) {
    if (Used.count(S))
      continue;
    Register Q = getHBNGSendPseudoRegs(*S)[0];
    // Not a send a later access still pairs with.
    if (Avail.lookup(Q) == S && neededLater(MI, Q))
      continue;

    auto AfterS = std::next(MachineBasicBlock::iterator(S));
    MachineBasicBlock::iterator LastDef = MBB.end();
    for (auto I = AfterS; I != MI.getIterator(); ++I)
      if (R != AArch64::XZR && I->modifiesRegister(R, TRI))
        LastDef = I;
    if (LastDef != MBB.end()) {
      Register Page = S->getOperand(0).getReg();
      auto Moved = make_range(AfterS, std::next(LastDef));
      if (llvm::any_of(Moved, [&](const MachineInstr &J) {
            return J.modifiesRegister(Page, TRI);
          }))
        continue;
      // The send now reads the page after these uses: none may kill it.
      for (MachineInstr &J : Moved)
        for (MachineOperand &MO : J.operands())
          if (MO.isReg() && MO.isUse() && MO.getReg() == Page)
            MO.setIsKill(false);
      MBB.splice(std::next(LastDef), &MBB, S->getIterator());
      S->getOperand(0).setIsKill(false);
    }

    if (Avail.lookup(Q) == S)
      Avail.erase(Q); // it no longer sends Q
    S->setDesc(TII->get(AArch64::HBNG_SEND)); // R is an X register
    S->getOperand(1).setReg(R);
    S->getOperand(1).setIsKill(false); // MI reads R after it
    return S;
  }
  return nullptr;
}

// MI's values pair with the nearest preceding send of each register, which
// no instruction in between writes; a send of the window nothing pairs with
// is given a missing one, and a fallback sends the others right before MI.
void AArch64HBNGCheckSends::checkBefore(MachineInstr &MI,
                                        ArrayRef<Register> Regs) {
  if (Regs.empty())
    return;
  // The sends there first, so that a repair does not take one of them.
  SmallVector<Register, 4> Absent;
  for (Register R : Regs) {
    if (MachineInstr *S = Avail.lookup(R))
      Used.insert(S);
    else
      Absent.push_back(R);
  }
  SmallVector<Register, 4> Missing;
  for (Register R : Absent) {
    MachineInstr *S = Avail.lookup(R); // repaired for an earlier copy of R
    if (!S && (S = repairFromWindow(MI, R))) {
      ++NumSendsRepaired;
      Avail[R] = S;
    }
    if (S)
      Used.insert(S);
    else
      Missing.push_back(R);
  }
  if (!Missing.empty())
    insertFallback(*MI.getParent(), MI.getIterator(), MI, Missing);
  Window.clear();
}

// The point after the sends that follow MI and pair with it (a
// store-exclusive's), where a fallback for MI goes.
MachineBasicBlock::iterator
AArch64HBNGCheckSends::afterOwnSends(MachineInstr &MI) {
  auto Where = std::next(MI.getIterator());
  while (Where != MI.getParent()->end() && isHBNGSendPseudo(*Where) &&
         Used.count(&*Where))
    ++Where;
  return Where;
}

// A store-exclusive sends after itself: its values pair with the nearest
// following send of each register that neither it nor an instruction in
// between writes (the one the pass before register allocation inserted),
// else with a preceding one. A fallback sends the missing ones right after
// MI.
void AArch64HBNGCheckSends::checkAfter(MachineInstr &MI,
                                       ArrayRef<Register> Regs) {
  if (Regs.empty())
    return;
  MachineBasicBlock &MBB = *MI.getParent();
  SmallVector<Register, 4> Missing;
  SmallPtrSet<MachineInstr *, 4> Taken;
  for (Register R : Regs) {
    MachineInstr *Found = nullptr;
    if (R == AArch64::XZR || !MI.modifiesRegister(R, TRI))
      for (auto I = std::next(MI.getIterator()); I != MBB.end(); ++I) {
        if (isHBNGSendPseudo(*I) && !Taken.count(&*I) &&
            getHBNGSendPseudoRegs(*I)[0] == R) {
          Found = &*I;
          break;
        }
        if (R != AArch64::XZR && I->modifiesRegister(R, TRI))
          break;
      }
    if (!Found)
      Found = Avail.lookup(R);
    if (Found) {
      Taken.insert(Found);
      Used.insert(Found);
    } else {
      Missing.push_back(R);
    }
  }
  if (!Missing.empty()) {
    // MI may have killed the registers sent after it.
    for (MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.isUse() && llvm::is_contained(Missing, MO.getReg()))
        MO.setIsKill(false);
    insertFallback(MBB, afterOwnSends(MI), MI, Missing);
  }
  Window.clear();
}

// Sends Regs at Where through a register holding the page, marks them used
// and notes them as the nearest sends of their registers.
void AArch64HBNGCheckSends::insertFallback(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Where,
                                           const MachineInstr &MI,
                                           ArrayRef<Register> Regs) {
  const DebugLoc &DL = MI.getDebugLoc();
  bool After = Where != MI.getIterator();
  ++NumFallbacks;
  NumFallbackRegs += Regs.size();

  // A register is never chosen for the page if the annotations track it (FP)
  // or it is reserved (SP, or a register reserved with -ffixed-xN).
  auto Candidate = [&](MCPhysReg R) {
    return R != AArch64::FP && !MRI->isReserved(R) &&
           !llvm::is_contained(Regs, R);
  };

  // The page: a register still holding it (not written since, nor by MI when
  // sending after it), else a free one, else one saved on the stack.
  Register Page;
  bool Spill = false;
  if (KnownPage && !(After && MI.modifiesRegister(KnownPage, TRI))) {
    Page = KnownPage;
    ++NumFbPageReused;
    // Its last use may have killed it.
    for (auto I = Where; I != MBB.begin();) {
      --I;
      bool Defines = I->modifiesRegister(Page, TRI);
      for (MachineOperand &MO : I->operands())
        if (MO.isReg() && MO.isUse() && MO.getReg() == Page)
          MO.setIsKill(false);
      if (Defines)
        break;
    }
  } else {
    LivePhysRegs Live(*TRI);
    Live.addLiveOuts(MBB);
    for (auto I = MBB.end(); I != Where;)
      Live.stepBackward(*--I);
    for (MCPhysReg R : PageRegOrder)
      if (Candidate(R) && Live.available(*MRI, R)) {
        Page = R;
        break;
      }
    if (Page) {
      ++NumFbPageScratch;
    } else {
      for (MCPhysReg R : PageRegOrder)
        if (Candidate(R)) {
          Page = R;
          break;
        }
      assert(Page && "HBNG: no register to hold the channel page");
      Spill = true;
      ++NumFbSpills;
      Inserted.insert(
          BuildMI(MBB, Where, DL, TII->get(AArch64::STRXpre), AArch64::SP)
              .addReg(Page)
              .addReg(AArch64::SP)
              .addImm(-16));
    }
    Inserted.insert(BuildMI(MBB, Where, DL, TII->get(AArch64::ADRP), Page)
                        .addExternalSymbol(HBNGChannelSymbol,
                                           AArch64II::MO_PAGE));
    // The adrp (and the reload) write the register: its earlier send no
    // longer pairs.
    if (MachineInstr *&S = Avail[Page])
      S = nullptr;
  }

  for (Register R : Regs) {
    MachineInstr *S = BuildMI(MBB, Where, DL, TII->get(AArch64::HBNG_SEND))
                          .addReg(Page)
                          .addReg(R);
    Used.insert(S);
    Inserted.insert(S);
    Avail[R] = S;
  }

  if (Spill)
    Inserted.insert(BuildMI(MBB, Where, DL, TII->get(AArch64::LDRXpost))
                        .addDef(AArch64::SP)
                        .addDef(Page)
                        .addReg(AArch64::SP)
                        .addImm(16));
  else
    KnownPage = Page;
}

// Two adjacent sends through the same page register become one pair: the
// same values, received in the same order, in one store.
void AArch64HBNGCheckSends::formPairs(MachineBasicBlock &MBB) {
  for (auto I = MBB.begin(); I != MBB.end();) {
    MachineInstr &First = *I++;
    if (!isHBNGSendPseudo(First) || I == MBB.end() || !isHBNGSendPseudo(*I) ||
        I->getOperand(0).getReg() != First.getOperand(0).getReg())
      continue;
    MachineInstr &Second = *I++;
    // A W register send (HBNG_SEND_W) is paired as its X register.
    Register A = getHBNGSendPseudoRegs(First)[0];
    Register B = getHBNGSendPseudoRegs(Second)[0];
    BuildMI(MBB, First.getIterator(), First.getDebugLoc(),
            TII->get(AArch64::HBNG_SEND_PAIR))
        .addReg(First.getOperand(0).getReg(),
                getKillRegState(Second.getOperand(0).isKill()))
        .addReg(A, getKillRegState(First.getOperand(1).isKill()))
        .addReg(B, getKillRegState(Second.getOperand(1).isKill()));
    First.eraseFromParent();
    Second.eraseFromParent();
  }
}

// Every value an annotation refers to (<Xn>, an address register, a source
// of SP, a new value of FP) must pair with a send: the nearest preceding send
// of Xn in the block (the annotation pass's: a machine block, also ended
// after a call or an svc) that no instruction in between writes, else, for a
// store-exclusive, the nearest following one. The pass checks each in
// program order, gives a missing register to a send nothing pairs with or
// inserts a fallback, then removes the sends nothing pairs with and forms the
// pairs.
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
    Used.clear();
    Inserted.clear();
    Window.clear();
    KnownPage = Register();
    // What a fallback inserts after MI may or may not be visited: it is
    // skipped, insertFallback notes it.
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      if (Inserted.count(&MI))
        continue;
      // The pass runs before the bundles are unpacked.
      assert(!bundleHasHBNGAccess(MI) && "HBNG: load or store inside a bundle");

      if (isHBNGSendPseudo(MI)) {
        noteSend(MI);
        continue;
      }

      if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI)) {
        SmallVector<Register, 4> Regs = addressSends(MI, *A);
        // Status >= 0 describes a store-exclusive: it sends after itself.
        if (A->Status >= 0)
          checkAfter(MI, Regs);
        else
          checkBefore(MI, Regs);
      }

      // SP written from a register: the annotation refers to its value.
      if (std::optional<HBNGSPWrite> W = getHBNGSPWrite(MI, TRI))
        checkBefore(MI, W->Sends);

      noteWrites(MI);

      // FP written with a value the annotations cannot compute (a reload):
      // the new value only exists after MI, so it is always a fallback, right
      // after MI and the sends that pair with MI. Its group pairs <X29> with
      // itself.
      if (isHBNGFPSentAfter(MI, TRI)) {
        assert(!MI.isTerminator() && "HBNG: FP written by a terminator");
        Register FP = AArch64::FP;
        insertFallback(MBB, afterOwnSends(MI), MI, FP);
      }

      if (isChannelPage(MI))
        KnownPage = MI.getOperand(0).getReg();

      if (endsAnnotationBlock(MI)) {
        Avail.clear();
        Window.clear();
      }
    }

    // The sends nothing pairs with: their access is gone, or uses other
    // registers.
    for (MachineInstr &MI : make_early_inc_range(MBB))
      if (isHBNGSendPseudo(MI) && !Used.count(&MI)) {
        MI.eraseFromParent();
        ++NumSendsRemoved;
      }

    formPairs(MBB);

    for (MachineInstr &MI : MBB) {
      if (isHBNGSendPseudo(MI)) {
        ++NumFinalSendInstrs;
        if (MI.getOpcode() == AArch64::HBNG_SEND_PAIR)
          ++NumFinalSendPairs;
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
