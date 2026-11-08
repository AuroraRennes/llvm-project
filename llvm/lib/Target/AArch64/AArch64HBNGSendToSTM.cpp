#include "AArch64.h"
#include "AArch64HBNGMemAccess.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"



using namespace llvm;

#define DEBUG_TYPE "hbng-send-stm"
#define TARGET_REG HBNGSendReg

namespace {
class AArch64HBNGSendToSTM : public MachineFunctionPass {
    const AArch64InstrInfo *TII;
    const TargetRegisterInfo *TRI;
public:
    static char ID;
    AArch64HBNGSendToSTM() : MachineFunctionPass(ID) {
        initializeAArch64HBNGSendToSTMPass(*PassRegistry::getPassRegistry());
    }

    // Insert the sends of the address registers of a load/store
    void insertStoreForAddressOperand(MachineBasicBlock &MBB, MachineInstr &MI,
                                      const HBNGMemAccess &A);

    // Insert sends of Regs at Where, two by two
    void insertSends(MachineBasicBlock &MBB, MachineBasicBlock::iterator Where,
                     const DebugLoc &DL, ArrayRef<Register> Regs);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
        return "AArch64 HBNG Send To STM";
    }
};

char AArch64HBNGSendToSTM::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGSendToSTM, DEBUG_TYPE, "HardBlare-NG Send to STM pass", false, false)


//===----------------------------------------------------------------------===//
// Main insertion function

// Sends, before MI, the address registers each of its annotation lines refers
// to: the base (unless it is SP or FP, whose values the annotations track, see
// isHBNGTrackedReg) and the register offset, once per data register (twice for a
// pair). A W register offset is sent as its X register. The registers are
// sent two by two with STP, the second one at the lower address, which the STM
// emits first, or alone with STR.
//
// The sends of a store-exclusive go right after it instead: before, they would
// be explicit stores between the load-exclusive and the store-exclusive, which
// the architecture allows to clear the exclusive monitor, so the store could
// fail on every retry. After it, they keep their order among the sends (the
// base register is unchanged: the status register cannot be the base), and a
// failed store-exclusive that loops back runs its send again with its
// annotation.
void AArch64HBNGSendToSTM::insertStoreForAddressOperand(
    MachineBasicBlock &MBB, MachineInstr &MI, const HBNGMemAccess &A) {
    LLVM_DEBUG(dbgs() << "Setting STM send for instruction: "
                      << TII->getName(MI.getOpcode()) << "\n");

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

    MachineBasicBlock::iterator Where = MI.getIterator();
    if (A.Status >= 0) // store-exclusive
        Where = std::next(Where);
    insertSends(MBB, Where, MI.getDebugLoc(), Sends);
}

// The registers are sent two by two with STP, the second one at the lower
// address, which the STM emits first, or alone with STR.
void AArch64HBNGSendToSTM::insertSends(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator Where,
                                       const DebugLoc &DL,
                                       ArrayRef<Register> Regs) {
    for (unsigned I = 0; I < Regs.size(); I += 2) {
        if (I + 1 < Regs.size())
            BuildMI(MBB, Where, DL, TII->get(AArch64::STPXi))
                .addReg(Regs[I + 1])
                .addReg(Regs[I])
                .addReg(TARGET_REG)
                .addImm(0);
        else
            BuildMI(MBB, Where, DL, TII->get(AArch64::STRXui))
                .addReg(Regs[I])
                .addReg(TARGET_REG)
                .addImm(0);
    }
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGSendToSTM::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Send To STM *****\n");

    // Early bailout if the function has attribute hbng_no_instr
    const Function &F = MF.getFunction();
    if (F.hasFnAttribute("hbng_no_instr")) {
      LLVM_DEBUG(dbgs() << "Skipping instrumentation for " << F.getName() << "\n");
      return false;
    }


    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();

    // The sends store through x18, which no other code may write.
    if (!STI.isXRegisterReserved(18))
        reportFatalUsageError("HBNG: x18 holds the STM address and must be "
                              "reserved (-ffixed-x18), in function '" +
                              MF.getName() + "'");

    bool Changed = false;

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
        LLVM_DEBUG(dbgs() << "block: " << MF.getName() << "\n");
        // Early increment: the sends inserted after an instruction are not
        // visited (they are stores themselves).
        for (MachineInstr &MI : make_early_inc_range(MBB)) {
            // The pass runs before the bundles are unpacked. AArch64 bundles
            // (SVE movprfx, calls with a marker or a BTI) hold no load or
            // store; the sends would otherwise have to go inside them.
            assert((!MI.isBundle() ||
                    llvm::none_of(make_range(std::next(MI.getIterator()),
                                             getBundleEnd(MI.getIterator())),
                                  [](const MachineInstr &BI) {
                                      return getHBNGMemAccess(BI).has_value();
                                  })) &&
                   "HBNG: load or store inside a bundle");

            // The address registers of the loads/stores the annotation pass
            // describes, before them
            if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI)) {
                insertStoreForAddressOperand(MBB, MI, *A);
                Changed = true;
            }

            // The sources of a write of SP the annotations cannot compute
            // from tracked registers, before it
            if (std::optional<HBNGSPWrite> W = getHBNGSPWrite(MI, TRI);
                W && !W->Sends.empty()) {
                insertSends(MBB, MI.getIterator(), MI.getDebugLoc(), W->Sends);
                Changed = true;
            }

            // The new value of FP, when the annotations cannot compute it,
            // right after MI (after a store-exclusive's own sends): it only
            // exists once MI has run, e.g. the epilogue's reload of x29.
            if (isHBNGFPSentAfter(MI, TRI)) {
                assert(!MI.isTerminator() && "HBNG: FP written by a terminator");
                MachineBasicBlock::iterator Where = std::next(MI.getIterator());
                while (Where != MBB.end() && isHBNGSend(*Where))
                    ++Where;
                Register FP = AArch64::FP;
                insertSends(MBB, Where, MI.getDebugLoc(), FP);
                Changed = true;
            }
        }
    }

    return Changed;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGSendToSTMPass() {
    return new AArch64HBNGSendToSTM();
}
