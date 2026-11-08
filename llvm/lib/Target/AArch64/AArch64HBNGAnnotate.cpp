#include "AArch64.h"
#include "AArch64HBNGMemAccess.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
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
#include "llvm/Support/raw_ostream.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"

using namespace llvm;

#define DEBUG_TYPE "hbng-annotate"

//===----------------------------------------------------------------------===//
// Helper generator functions



//===----------------------------------------------------------------------===//
// Main class definition

enum Size {
  UNUSED = 0,
  BYTE = 8,
  HALF = 16,
  WORD = 32,
  DOUB = 64,
  QUAD = 128
};

namespace {
class AArch64HBNGAnnotate : public MachineFunctionPass {
    const AArch64InstrInfo *TII;
    const TargetRegisterInfo *TRI;
    const MachineRegisterInfo *MRI;

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
    }

    // Generate annotations for all instructions in a given machine function
    std::vector<std::string> generateAnnotation(MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void printStrOperand(MachineOperand &MO, raw_string_ostream &OS);
    std::vector<std::string> genRdOneOperandAnnotation(MachineInstr &MI, StringRef Op);
    std::vector<std::string> genRdTwoOperandsAnnotation(MachineInstr &MI, StringRef Op);
    std::vector<std::string> genRdThreeOperandsAnnotation(MachineInstr &MI, StringRef Op);
    std::vector<std::string> genRdImmAnnotation(MachineInstr &MI, StringRef Op, bool Sticky);
    std::vector<std::string> genRdPCAnnotation(MachineInstr &MI, StringRef Op, bool Page);
    std::vector<std::string> genLinkAnnotation();
    std::vector<std::string> genMemAnnotation(MachineInstr &MI, const HBNGMemAccess &A);
    std::vector<std::string> genZero(MachineInstr &MI);
    std::vector<std::string> genInsertAnnotation(MachineInstr &MI);
    bool writesZeroReg(MachineInstr &MI);
};

char AArch64HBNGAnnotate::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGAnnotate, DEBUG_TYPE, "HardBlare-NG annotation pass", false, false)


//===----------------------------------------------------------------------===//
// Helper functions

static bool containsSVC(MachineInstr &MI) {
  bool IsSVC = false;
  if (MI.isInlineAsm()) {
    StringRef AsmStr = MI.getOperand(0).getSymbolName();
    if (AsmStr.contains("svc")) {
      IsSVC = true;
    }
  }
  return IsSVC;
}

void AArch64HBNGAnnotate::printStrOperand(MachineOperand &MO, raw_string_ostream &OS) {
  if (MO.isReg()) {
    unsigned Reg = MO.getReg();
    OS << TRI->getRegAsmName(Reg);
  } else if (MO.isImm()) {
    OS << "Imm(" << MO.getImm() << ")";
  } else {
    OS << "UOp";
  }
}

//===----------------------------------------------------------------------===//
// Specific annotation functions
//
// The condition flags carry no tag. A comparison reaches the data only through
// a branch or a conditional select/set, which are implicit flows and out of
// scope of the policy: a value computed with cset is clean, and a csel takes
// the tags of both data operands but not of the condition. So flag-setting
// instructions annotate only their destination register (nothing for
// cmp/cmn/tst/fcmp/ccmp), the carry of adc/sbc is not propagated, and branches
// carry no tag into PC.

// A write to WZR/XZR (cmp, cmn, tst) has no effect on the tags.
bool AArch64HBNGAnnotate::writesZeroReg(MachineInstr &MI) {
  MachineOperand &Rd = MI.getOperand(0);
  return Rd.isReg() && Rd.isDef() &&
         (Rd.getReg() == AArch64::WZR || Rd.getReg() == AArch64::XZR);
}

std::vector<std::string> AArch64HBNGAnnotate::genZero(MachineInstr &MI) {
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  // FIXME: Passing imm(0) might not be enough to mean REPLACE the previous tag with 0
  OS << " <- Imm(0)";

  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdOneOperandAnnotation(MachineInstr &MI, StringRef Op) {
  // Annotations vector
  std::vector<std::string> Annotations = {};
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  printStrOperand(MI.getOperand(1), OS);
  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdTwoOperandsAnnotation(MachineInstr &MI, StringRef Op) {
  if (writesZeroReg(MI))
    return {};
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  printStrOperand(MI.getOperand(1), OS);
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(2), OS);
  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}


std::vector<std::string> AArch64HBNGAnnotate::genRdThreeOperandsAnnotation(MachineInstr &MI, StringRef Op) {
  if (writesZeroReg(MI))
    return {};
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  printStrOperand(MI.getOperand(1), OS);
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(2), OS);
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(3), OS);
  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdImmAnnotation(MachineInstr &MI, StringRef Op, bool Sticky) {
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  if (Sticky) {
    printStrOperand(MI.getOperand(0), OS);
    OS << " " << Op << " ";
  }
  printStrOperand(MI.getOperand(1), OS);

  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdPCAnnotation(MachineInstr &MI, StringRef Op, bool Page) {
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- PC";

  // Propagating the code memory tag
#ifdef false
  OS << " " << "[<PC> + ";
  printStrOperand(MI.getOperand(1), OS)
  if (Page) {
    OS << " << 12"
  }
  OS << "]"
#endif

  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

// Lane insert (INS, also written MOV): Vd <- Vd bit Src. The register keeps
// the tags of its other lanes, so the destination is also a source.
std::vector<std::string> AArch64HBNGAnnotate::genInsertAnnotation(MachineInstr &MI) {
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  printStrOperand(MI.getOperand(1), OS);
  OS << " bit ";
  printStrOperand(MI.getOperand(3), OS);
  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

// A call writes the return address, a constant, into LR.
std::vector<std::string> AArch64HBNGAnnotate::genLinkAnnotation() {
  return {"LR <- Imm(0)\n"};
}

// Annotations of a load or store, in the form (one line per data register,
// then the base write-back, then the store-exclusive status):
//   Rt <- [<Rn> + Off + k]_Bits loa Rn loa Off      (load)
//   [<Rn> + Off + k]_Bits <- Rt sto Rn sto Off      (store)
//   Rn <- Rn loa Off                                (pre/post write-back)
//   Ws <- Imm(0)                                    (store-exclusive status)
// Off is in bytes: "S*Imm(i)" for an immediate in units of the access size S,
// "Imm(i)" for a byte immediate, "<Rm> ext amount" for a register offset, and
// "Imm(0)" for a post-indexed access (the address is the base before the
// write-back). k is the byte offset of the second register of a pair. Bits is
// the width of one data register's access.
std::vector<std::string> AArch64HBNGAnnotate::genMemAnnotation(
    MachineInstr &MI, const HBNGMemAccess &A) {
  std::vector<std::string> Annotations;
  StringRef Op = A.IsStore ? "sto" : "loa";
  MachineOperand &Base = MI.getOperand(A.Base);

  // The offset as written in the instruction (also used for the write-back).
  std::string Off;
  raw_string_ostream OSOff(Off);
  if (A.Offset < 0) {
    OSOff << "Imm(0)";
  } else if (A.isRegOffset()) {
    MachineOperand &Rm = MI.getOperand(A.Offset);
    bool Signed = MI.getOperand(A.Offset + 1).getImm();
    bool DoShift = MI.getOperand(A.Offset + 2).getImm();
    AArch64_AM::ShiftExtendType Ext =
        A.Mode == HBNGMemAccess::RegOffsetW
            ? (Signed ? AArch64_AM::SXTW : AArch64_AM::UXTW)
            : (Signed ? AArch64_AM::SXTX : AArch64_AM::UXTX);
    OSOff << "<";
    printStrOperand(Rm, OSOff);
    OSOff << "> " << AArch64_AM::getShiftExtendName(Ext) << " "
          << (DoShift ? Log2_32(A.Size) : 0);
  } else {
    if (A.isScaledImm())
      OSOff << A.Size << "*";
    printStrOperand(MI.getOperand(A.Offset), OSOff);
  }
  // The offset added to the base to form the address.
  std::string AddrOff = A.Mode == HBNGMemAccess::PostIndex ? "Imm(0)" : Off;

  for (unsigned I = 0; I < A.NumData; ++I) {
    std::string Addr;
    raw_string_ostream OSAddr(Addr);
    OSAddr << "[<";
    printStrOperand(Base, OSAddr);
    OSAddr << "> + " << AddrOff;
    if (I)
      OSAddr << " + " << I * A.Size;
    OSAddr << "]_" << A.Size * 8;

    std::string Result;
    raw_string_ostream OS(Result);
    MachineOperand &Rt = MI.getOperand(A.FirstData + I);
    if (A.IsStore) {
      OS << Addr << " <- ";
      printStrOperand(Rt, OS);
    } else {
      printStrOperand(Rt, OS);
      OS << " <- " << Addr;
    }
    OS << " " << Op << " ";
    printStrOperand(Base, OS);
    if (A.Offset >= 0) {
      OS << " " << Op << " ";
      printStrOperand(MI.getOperand(A.Offset), OS);
    }
    LLVM_DEBUG(dbgs() << Result << "\n");
    Annotations.push_back(Result + "\n");
  }

  if (A.hasWriteback()) {
    std::string Result;
    raw_string_ostream OS(Result);
    printStrOperand(Base, OS);
    OS << " <- ";
    printStrOperand(Base, OS);
    OS << " " << Op << " " << Off;
    LLVM_DEBUG(dbgs() << Result << "\n");
    Annotations.push_back(Result + "\n");
  }

  if (A.Status >= 0) {
    std::string Result;
    raw_string_ostream OS(Result);
    printStrOperand(MI.getOperand(A.Status), OS);
    OS << " <- Imm(0)";
    LLVM_DEBUG(dbgs() << Result << "\n");
    Annotations.push_back(Result + "\n");
  }

  return Annotations;
}

//===----------------------------------------------------------------------===//
// Main annotation generation function

std::vector<std::string> AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI) {
  std::vector<std::string> Annotations = {};

  std::string Unsupported;
  raw_string_ostream OS(Unsupported);
  OS << "Unsupported: " << TII->getName(MI.getOpcode());

  if (TII->isGPRZero(MI)) {
    return genZero(MI);
  }

  if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI))
    return genMemAnnotation(MI, *A);

  switch (MI.getOpcode()) {
    //=== ARITHMETIC ===//
    // One Register or Immediate
    case AArch64::NEGv2i32: case AArch64::NEGv4i16: case AArch64::NEGv4i32:
    case AArch64::NEGv8i8: case AArch64::NEGv8i16: case AArch64::NEGv16i8:
    case AArch64::FNEGSr: case AArch64::FNEGDr:
    case AArch64::FNEGv2f32: case AArch64::FNEGv2f64:
    case AArch64::FNEGv4f16: case AArch64::FNEGv4f32:
    case AArch64::FNEGv8f16:
    case AArch64::FABSSr: case AArch64::FABSDr:
    case AArch64::FABSv2f32: case AArch64::FABSv2f64: case AArch64::FABSv4f32:
      Annotations = genRdOneOperandAnnotation(MI, "ari");
      break;
    // Two registers
    case AArch64::ADDXrr: case AArch64::ADDXrx: case AArch64::ADDXrs:
    case AArch64::ADDWrr: case AArch64::ADDWrx: case AArch64::ADDWrs:
    case AArch64::ADDv2i64: case AArch64::ADDVv8i8v:
    case AArch64::FADDSrr: case AArch64::FADDDrr:
    case AArch64::FADDv2f32: case AArch64::FADDv2f64:
    case AArch64::FADDv4f16: case AArch64::FADDv4f32: case AArch64::FADDv8f16:
      Annotations = genRdTwoOperandsAnnotation(MI, "add");
      break;
    case AArch64::SUBXrr: case AArch64::SUBXrx: case AArch64::SUBXrs:
    case AArch64::SUBWrr: case AArch64::SUBWrx: case AArch64::SUBWrs:
    case AArch64::SUBv2i64:
    case AArch64::FSUBDrr: case AArch64::FSUBSrr: case AArch64::FSUBv4f32:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub");
      break;
    case AArch64::ADDSXrr: case AArch64::ADDSXrx: case AArch64::ADDSXrs:
    case AArch64::ADDSWrr: case AArch64::ADDSWrx: case AArch64::ADDSWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "add");
      break;
    case AArch64::SUBSXrr: case AArch64::SUBSXrx: case AArch64::SUBSXrs:
    case AArch64::SUBSWrr: case AArch64::SUBSWrx: case AArch64::SUBSWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub");
      break;
    case AArch64::ADCSXr: case AArch64::ADCSWr:
    case AArch64::SBCSXr: case AArch64::SBCSWr:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari");
      break;
    case AArch64::ADCXr: case AArch64::ADCWr:
    case AArch64::SBCXr: case AArch64::SBCWr:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari");
      break;
    case AArch64::UMULHrr: case AArch64::SMULHrr:
    case AArch64::MULv2i32: case AArch64::MULv4i32:
    case AArch64::FMULSrr: case AArch64::FMULDrr:
    case AArch64::FNMULSrr: case AArch64::FNMULDrr:
    case AArch64::FMULv2f32: case AArch64::FMULv2f64:
    case AArch64::FMULv4f16: case AArch64::FMULv4f32:
    case AArch64::FMULv8f16:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari");
      break;
    // Register and Immediate
    case AArch64::ADDXri: case AArch64::ADDWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "add");
      break;
    case AArch64::SUBXri: case AArch64::SUBWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub");
      break;
    case AArch64::ADDSXri: case AArch64::ADDSWri:
    case AArch64::SUBSXri: case AArch64::SUBSWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari");
      break;
    case AArch64::UDIVWr: case AArch64::UDIVXr:
    case AArch64::SDIVWr: case AArch64::SDIVXr:
    case AArch64::FDIVSrr: case AArch64::FDIVDrr:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari");
      break;
    // Three registers
    case AArch64::MADDWrrr: case AArch64::MADDXrrr:
    case AArch64::MSUBWrrr: case AArch64::MSUBXrrr:
    case AArch64::UMSUBLrrr: case AArch64::UMADDLrrr:
    case AArch64::SMSUBLrrr: case AArch64::SMADDLrrr:
    case AArch64::FMADDHrrr: case AArch64::FMADDSrrr: case AArch64::FMADDDrrr:
    case AArch64::FNMADDHrrr: case AArch64::FNMADDSrrr: case AArch64::FNMADDDrrr:
    case AArch64::FMSUBHrrr: case AArch64::FMSUBSrrr: case AArch64::FMSUBDrrr:
    case AArch64::FNMSUBHrrr: case AArch64::FNMSUBSrrr: case AArch64::FNMSUBDrrr:
    case AArch64::FMLAv1i32_indexed: case AArch64::FMLAv1i64_indexed:
    case AArch64::FMULv1i32_indexed: case AArch64::FMULv1i64_indexed:
      // TODO: Check for the carry?
      Annotations = genRdThreeOperandsAnnotation(MI, "ari");
      break;

    //=== FP COMPARISONS ===//
    case AArch64::FMAXNMSrr: case AArch64::FMAXNMDrr:
    case AArch64::FMINNMSrr: case AArch64::FMINNMDrr:
      Annotations = genRdTwoOperandsAnnotation(MI, "cmp");
      break;
    case AArch64::FCMEQv2f32: case AArch64::FCMEQv2f64: case AArch64::FCMEQv4f32:
      Annotations = genRdTwoOperandsAnnotation(MI, "cmp");
      break;
    case AArch64::UMAXVv4i32v:
      Annotations = genRdOneOperandAnnotation(MI, "cmp");
      break;

    //=== FP CONVERSION ===//
    case AArch64::FCVTDSr: case AArch64::FCVTSDr:
    case AArch64::FCVTZSUWSr:
    case AArch64::FCVTZSUWDr: case AArch64::FCVTZUUWDr:
    case AArch64::SCVTFUWSri: case AArch64::SCVTFUWDri: case AArch64::SCVTFUXSri: case AArch64::SCVTFUXDri:
    case AArch64::UCVTFUWSri: case AArch64::UCVTFUWDri: case AArch64::UCVTFUXSri: case AArch64::UCVTFUXDri:
    case AArch64::SCVTFv1i16: case AArch64::SCVTFv1i32: case AArch64::SCVTFv1i64:
    case AArch64::UCVTFv2f32: case AArch64::UCVTFv2f64:
      Annotations = genRdOneOperandAnnotation(MI, "cvt");
      break;

    //=== ADDR COMPUTATION ===//
    case AArch64::ADR:
      Annotations = genRdPCAnnotation(MI, "adr", /*Page=*/false);
      break;
    case AArch64::ADRP:
      Annotations = genRdPCAnnotation(MI, "adr", /*Page=*/true);
      break;
    //=== LOGICAL ===//
    // reg
    case AArch64::ANDXrr: case AArch64::ANDXrs: case AArch64::ANDWrr: case AArch64::ANDWrs:
    case AArch64::ANDv16i8: case AArch64::ANDv8i8:
    case AArch64::BICXrr: case AArch64::BICXrs: case AArch64::BICWrr: case AArch64::BICWrs:
    case AArch64::EORXrr: case AArch64::EORXrs: case AArch64::EORWrr: case AArch64::EORWrs:
    case AArch64::EORv8i8: case AArch64::EORv16i8:
    case AArch64::EONXrr: case AArch64::EONXrs: case AArch64::EONWrr: case AArch64::EONWrs:
    case AArch64::ORNXrr: case AArch64::ORNXrs: case AArch64::ORNWrr: case AArch64::ORNWrs:
    case AArch64::ORRXrr: case AArch64::ORRXrs: case AArch64::ORRWrr: case AArch64::ORRWrs:
    case AArch64::ORRv16i8: case AArch64::ORRv8i8:
    // imm
    case AArch64::EORWri: case AArch64::EORXri:
    case AArch64::ORRXri: case AArch64::ORRWri:
    case AArch64::ANDXri: case AArch64::ANDWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "log");
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs: case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs: case AArch64::BICSWrr: case AArch64::BICSWrs:
    case AArch64::ANDSXri: case AArch64::ANDSWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "log");
      break;
    //=== SHIFTS ===//
    case AArch64::ASRVWr: case AArch64::ASRVXr:
    case AArch64::LSRVWr: case AArch64::LSRVXr:
    case AArch64::LSLVWr: case AArch64::LSLVXr:
    case AArch64::USHLLv2i32_shift: case AArch64::USHLv2i64:
      Annotations = genRdTwoOperandsAnnotation(MI, "shi");
      break;
    //=== MOVES ===//
    // Partial move
    case AArch64::MOVKWi: case AArch64::MOVKXi: case AArch64::FMOVv2f32_ns:
      Annotations = genRdImmAnnotation(MI, "mov", /*Sticky=*/true);
      break;
    // Total move
    case AArch64::MOVNWi: case AArch64::MOVNXi:
    case AArch64::MOVZXi: case AArch64::MOVZWi:
    case AArch64::MOVID: case AArch64::FMOVSi: case AArch64::FMOVDi:
    case AArch64::MOVIv2i32: case AArch64::MOVIv4i32:
    case AArch64::MVNIv2i32: case AArch64::MVNIv4i32:
    case AArch64::MVNIv2s_msl: case AArch64::MVNIv4s_msl:
    case AArch64::MOVIv2d_ns: case AArch64::MOVIv16b_ns:
      Annotations = genRdImmAnnotation(MI, "mov", /*Sticky=*/false);
      break;
    // Zero
    case AArch64::FMOVS0: case AArch64::FMOVD0:
      Annotations = genZero(MI);
      break;
    // Register move
    case AArch64::FMOVDr: case AArch64::FMOVDXr: case AArch64::FMOVXDr:
    case AArch64::FMOVSr: case AArch64::FMOVSWr: case AArch64::FMOVWSr:
    case AArch64::UMOVvi8_idx0:
      Annotations = genRdOneOperandAnnotation(MI, "mov");
      break;
    //=== BITMANIP ===//
    case AArch64::SBFMXri: case AArch64::SBFMWri:
    case AArch64::UBFMXri: case AArch64::UBFMWri:
    case AArch64::BFMWri: case AArch64::BFMXri:
    case AArch64::EXTRWrri: case AArch64::EXTRXrri:
    case AArch64::EXTv8i8: case AArch64::EXTv16i8:
      Annotations = genRdThreeOperandsAnnotation(MI, "bit");
      break;
    case AArch64::XTNv2i32: case AArch64::XTNv4i32:
      Annotations = genRdOneOperandAnnotation(MI, "bit");
      break;
    case AArch64::ZIP1v2i32: case AArch64::ZIP2v2i32:
    case AArch64::ZIP1v4i16: case AArch64::ZIP2v4i16:
    case AArch64::ZIP1v4i32: case AArch64::ZIP2v4i32:
    case AArch64::ZIP1v8i8: case AArch64::ZIP2v8i8:
    case AArch64::ZIP1v8i16: case AArch64::ZIP2v8i16:
    case AArch64::ZIP1v16i8: case AArch64::ZIP2v16i8:
      Annotations = genRdTwoOperandsAnnotation(MI, "bit");
      break;
    // Vector merges
    // (Vd, Vd tied, Vn, Vm): Vd's own bits are one of the three inputs.
    case AArch64::BIFv16i8: case AArch64::BIFv8i8: case AArch64::BITv16i8: case AArch64::BITv8i8:
    case AArch64::BSLv16i8: case AArch64::BSLv8i8:
      Annotations = genRdThreeOperandsAnnotation(MI, "bit");
      break;
    case AArch64::DUPi32: case AArch64::DUPi64:
      Annotations = genRdImmAnnotation(MI, "bit", false);
      break;
    case AArch64::DUPv2i32lane: case AArch64::DUPv2i64lane: case AArch64::DUPv4i32lane:
    case AArch64::DUPv2i32gpr: case AArch64::DUPv2i64gpr: case AArch64::DUPv4i32gpr:
      Annotations = genRdOneOperandAnnotation(MI, "bit");
      break;
    // (Vd, Vd tied, idx, Rn [, idx2]): one lane from Rn, the others kept.
    case AArch64::INSvi8gpr: case AArch64::INSvi16gpr:
    case AArch64::INSvi32gpr: case AArch64::INSvi64gpr:
    case AArch64::INSvi8lane: case AArch64::INSvi16lane:
    case AArch64::INSvi32lane: case AArch64::INSvi64lane:
      Annotations = genInsertAnnotation(MI);
      break;

    // Bit count and reverse
    case AArch64::CLZWr: case AArch64::CLZXr:
    case AArch64::RBITWr: case AArch64::RBITXr:
    case AArch64::REVWr: case AArch64::REVXr:
    case AArch64::REV32v4i16: case AArch64::REV32v8i16: case AArch64::REV32v8i8: case AArch64::REV32v16i8:
    case AArch64::REV64v4i16: case AArch64::REV64v8i16: case AArch64::REV64v2i32: case AArch64::REV64v4i32:
    case AArch64::CNTv8i8: case AArch64::CNTv16i8:
      Annotations = genRdOneOperandAnnotation(MI, "bit");
      break;
    //=== BRANCHES ===//
    // No tag flows into PC (see "Specific annotation functions"); the block
    // structure is given by the block table and the END annotations.
    case AArch64::B: case AArch64::BR: case AArch64::Bcc: case AArch64::RET:
    case AArch64::TBNZX: case AArch64::TBNZW: case AArch64::TBZW: case AArch64::TBZX:
    case AArch64::CBZW: case AArch64::CBZX: case AArch64::CBNZW: case AArch64::CBNZX:
    // Tail calls, pseudo instructions that become B/BR
    case AArch64::TCRETURNri: case AArch64::TCRETURNdi: case AArch64::TCRETURNriALL:
    case AArch64::TCRETURNrix16x17: case AArch64::TCRETURNrix17: case AArch64::TCRETURNrinotx16:
      break;
    case AArch64::BL: case AArch64::BLR:
      Annotations = genLinkAnnotation();
      break;

    //=== CONDITIONAL OPERATIONS ===//
    // Both data operands, not the condition.
    case AArch64::CSELWr: case AArch64::CSELXr: case AArch64::CSINCWr: case AArch64::CSINCXr:
    case AArch64::CSINVWr: case AArch64::CSINVXr: case AArch64::CSNEGWr: case AArch64::CSNEGXr:
    case AArch64::FCSELSrrr: case AArch64::FCSELDrrr:
      Annotations = genRdTwoOperandsAnnotation(MI, "cse");
      break;

    //=== COMPARISONS ===//
    // Write only the flags.
    case AArch64::FCMPSrr: case AArch64::FCMPDrr: case AArch64::FCMPSri: case AArch64::FCMPDri:
    case AArch64::FCMPESrr: case AArch64::FCMPEDrr: case AArch64::FCMPESri: case AArch64::FCMPEDri:
    case AArch64::CCMPWi: case AArch64::CCMPWr: case AArch64::CCMPXi: case AArch64::CCMPXr:
    case AArch64::CCMNWi: case AArch64::CCMNWr: case AArch64::CCMNXi: case AArch64::CCMNXr:
    case AArch64::FCCMPSrr: case AArch64::FCCMPDrr:
    case AArch64::FCCMPESrr: case AArch64::FCCMPEDrr:
      break;

    // Ignore
    case AArch64::INLINEASM:
      if (containsSVC(MI)) {
        Annotations.push_back("SVC\n");
      }
      break;
    case AArch64::CLREX:           // Cancel exclusive access
    case AArch64::CFI_INSTRUCTION:
    case AArch64::DBG_VALUE:
    case AArch64::DBG_VALUE_LIST:
    case AArch64::IMPLICIT_DEF:
    case AArch64::JUMP_TABLE_DEBUG_INFO:
    case AArch64::JumpTableDest16:
    case AArch64::JumpTableDest8:
    case AArch64::KILL:
    case AArch64::HBNG_ATOM_LABEL:
      break;
    default:
      Annotations.push_back(Unsupported + "\n");
      LLVM_DEBUG(dbgs() << Unsupported << "\n");
      break;
  }
  return Annotations;
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");

    // TODO: Would be nice but activating it breaks the generation of temporary symbols
    // const Function &F = MF.getFunction();
    // if (F.hasFnAttribute("hbng_no_instr")) {
    //   // FIXME: temporary labels are not emitted I dont know why
    //   for (auto &MBB : MF) MBB.setLabelMustBeEmitted();
    //   LLVM_DEBUG(dbgs() << "Skipping instrumentation for " << F.getName() << "\n");
    //   return false;
    // }


    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MRI = &MF.getRegInfo();
    // Function Info, the basic block table is stored here
    AArch64FunctionInfo *FI = MF.getInfo<AArch64FunctionInfo>();

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
      LLVM_DEBUG(dbgs() << "bblock: " << MBB.getName() << "\n");
      // Get the symbol of the basic block
      // Kept as a std::string: a Twine must not outlive the temporaries it
      // points to.
      std::string SymbolName = MBB.isEntryBlock()
        ? ("HBNG_" + MF.getFunction().getName()).str()
        : ("HBNG_" + MF.getFunction().getName() + "_n_" + Twine(MBB.getNumber())).str();
      MCSymbol *BBSym = MF.getContext().getOrCreateSymbol(SymbolName);
      MCSymbol *BBAnnotSym = MF.getContext().getOrCreateSymbol(SymbolName + "_annot");

      LLVM_DEBUG(dbgs() << "ri block name: " << BBSym->getName() << "\n");

      // Adds the basic block symbol and start offset of the annotations in the table
      HBNGAnnotationInfo BBAnnotationInfo;
      BBAnnotationInfo.BBSymbol = BBSym;
      BBAnnotationInfo.BBAnnotSymbol = BBAnnotSym;
      BBAnnotationInfo.Annotations = {};


      unsigned SplitIndex = 0;

      for (auto MIIt = MBB.begin(), End = MBB.end(); MIIt != End; ++MIIt) {
        MachineInstr &MI = *MIIt;
        unsigned int Opcode = MI.getOpcode();
        StringRef Mnemonic = TII->getName(Opcode);
        LLVM_DEBUG(dbgs() << "instr: " << Mnemonic << "\n");
        // Main generation function, creates AND STORES the annotation
        std::vector<std::string> InstrAnnotations = generateAnnotation(MI);
        BBAnnotationInfo.Annotations.insert(
          BBAnnotationInfo.Annotations.end(),
          InstrAnnotations.begin(),
          InstrAnnotations.end()
        );

        // Check if the current instruction is a branch/return/call etc. but the
        // basic block does not end on it. If this is the case, we have to emit
        // a symbol and push it to the bbt to comply to the CoreSight way of defining
        // Atoms.
        bool IsLastInstr = std::next(MIIt) == End;
        bool IsCtrlFlow = MI.isBranch() || MI.isReturn() || MI.isCall();
        if ((IsCtrlFlow || containsSVC(MI)) && !IsLastInstr) {
          BBAnnotationInfo.Annotations.push_back("END\n");
          FI->BBAnnotationInfos.push_back(BBAnnotationInfo);

          // Insert the pseudo instruction in the MBB
          BuildMI(MBB, std::next(MIIt), DebugLoc(), TII->get(AArch64::HBNG_ATOM_LABEL));

          // Create the new symbols, splitting the basic block and annotations
          std::string SplitBaseName = SymbolName + "_split_" + std::to_string(SplitIndex++);
          MCSymbol *SplitBBSym = MF.getContext().getOrCreateSymbol(SplitBaseName);
          MCSymbol *SplitAnnotSym = MF.getContext().getOrCreateSymbol(SplitBaseName + "_annot");
          // Reset the BBAnnotation
          BBAnnotationInfo.BBSymbol = SplitBBSym;
          BBAnnotationInfo.BBAnnotSymbol = SplitAnnotSym;
          BBAnnotationInfo.Annotations = {};

          LLVM_DEBUG(dbgs() << "Splitting the block, resetting info with: " << SplitBBSym->getName() << " and " << SplitAnnotSym->getName() << "\n");
        }
      }

      // Emit END annotation only if block ends in a branch
      if (!MBB.empty()) {
        MachineInstr &LastMI = MBB.back();
        if (LastMI.isBranch() || LastMI.isReturn() || LastMI.isCall() || containsSVC(LastMI)) {
          std::string EndAnnotation = "END\n";
          BBAnnotationInfo.Annotations.push_back(EndAnnotation);
        }
      }

      FI->BBAnnotationInfos.push_back(BBAnnotationInfo);
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}