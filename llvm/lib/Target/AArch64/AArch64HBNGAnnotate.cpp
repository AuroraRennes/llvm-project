#include "AArch64.h"
#include "AArch64HBNGAnnotation.h"
#include "AArch64HBNGMemAccess.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/StringExtras.h"
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
    // The terms of the function's annotations.
    hbng::TermPool Terms;

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
    }

    // The groups of MI, one per instruction it is encoded as (none for a
    // pseudo that emits nothing), each the transfer of that instruction.
    std::vector<hbng::Transfer> generateGroups(MachineInstr &MI);
    std::vector<hbng::Transfer> genInlineAsmGroups(MachineInstr &MI);
    hbng::Transfer genSends(MachineInstr &MI);
    // The transfer of an instruction encoded as one instruction.
    hbng::Transfer generateAnnotation(MachineInstr &MI);
    hbng::Transfer genTrackedValues(MachineInstr &MI);
    hbng::Transfer generateTagAnnotation(MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    hbng::LocList locsOf(Register R);
    void writeReg(hbng::Transfer &T, Register R, ArrayRef<hbng::Loc> Srcs,
                  bool Bytewise = false);
    void writeRegPer(hbng::Transfer &T, Register R,
                     ArrayRef<ArrayRef<hbng::Loc>> Groups, unsigned In,
                     unsigned Out, ArrayRef<ArrayRef<hbng::Loc>> Excl = {});
    void writeRegMap(hbng::Transfer &T, Register R,
                     ArrayRef<std::optional<hbng::Loc>> Map);
    hbng::Term termOf(Register R);
    void genDataflow(hbng::Transfer &T, MachineInstr &MI);
    void genClear(hbng::Transfer &T, MachineInstr &MI);
    std::pair<hbng::Term, int64_t> genAddress(MachineInstr &MI,
                                               const HBNGMemAccess &A);
    hbng::Transfer genMemAnnotation(MachineInstr &MI, const HBNGMemAccess &A);
    hbng::Transfer genVecMemAnnotation(MachineInstr &MI,
                                                 const HBNGMemAccess &A);
    std::optional<hbng::Transfer> genNeonAnnotation(MachineInstr &MI);
    unsigned regBytes(Register R);
    void setTrackedValue(hbng::Transfer &T, Register Dst, Register Src,
                         int64_t Imm);
    void genSPWrite(hbng::Transfer &T, MachineInstr &MI, const HBNGSPWrite &W);
};

char AArch64HBNGAnnotate::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGAnnotate, DEBUG_TYPE, "HardBlare-NG annotation pass", false, false)


//===----------------------------------------------------------------------===//
// Helper functions

// The mnemonics of the instructions in an inline asm statement, in order and in
// lower case: statements are separated by newlines and ';', and comments
// ("//"), labels ("1:") and directives (".text") are dropped.
static SmallVector<std::string, 4> getInlineAsmMnemonics(const MachineInstr &MI) {
  SmallVector<std::string, 4> Mnemonics;
  if (!MI.isInlineAsm())
    return Mnemonics;
  SmallVector<StringRef, 8> Lines;
  StringRef(MI.getOperand(0).getSymbolName()).split(Lines, '\n');
  for (StringRef Line : Lines) {
    SmallVector<StringRef, 4> Statements;
    Line.split("//").first.split(Statements, ';');
    for (StringRef Rest : Statements) {
      // The first token after the labels is the mnemonic.
      while (true) {
        auto [Token, Next] = getToken(Rest, " \t\r\v\f");
        Rest = Next;
        if (Token.empty())
          break;
        if (Token.ends_with(":"))
          continue; // label
        if (!Token.starts_with("."))
          Mnemonics.push_back(Token.lower());
        break;
      }
    }
  }
  return Mnemonics;
}

static bool containsSVC(MachineInstr &MI) {
  return llvm::is_contained(getInlineAsmMnemonics(MI), "svc");
}

//===----------------------------------------------------------------------===//
// The annotation language
//
// Each line is a statement: "<written> <- <sources>" for tags, "<SP> <- term"
// or "<FP> <- term" for the tracked values. Left is the exit, right the entry.
// Bare names are tags: X0-X28, FP, LR, SP (one tag each, a W register is its
// X), V<n>[lo:hi] (one tag per byte), and memory bytes M[<addr> .. +size].
// Angle brackets are values: <Xn> the register's value at the instruction's
// entry, which the checker pairs with a send, <SP> and <FP> the tracked ones.
// The sources are combined with OR, "0" is no source (a clear), immediates
// carry no tag and are left out.
//
// The condition flags carry no tag. A comparison reaches the data only through
// a branch or a conditional select/set, which are implicit flows and out of
// scope of the policy: a value computed with cset is clean, and a csel takes
// the tags of both data operands but not of the condition. So flag-setting
// instructions annotate only their destination register (nothing for
// cmp/cmn/tst/fcmp/ccmp), the carry of adc/sbc is not propagated, and branches
// carry no tag into PC.

static bool isFPR(Register R) {
  return AArch64::FPR128RegClass.contains(R) ||
         AArch64::FPR64RegClass.contains(R) ||
         AArch64::FPR32RegClass.contains(R) ||
         AArch64::FPR16RegClass.contains(R) ||
         AArch64::FPR8RegClass.contains(R);
}

// The locations of a source register: its tag (none for the zero register),
// or every byte of an FP/SIMD register.
hbng::LocList AArch64HBNGAnnotate::locsOf(Register R) {
  if (isFPR(R))
    return hbng::vBytes(TRI->getEncodingValue(R), 0, regBytes(R));
  if (R == AArch64::XZR || R == AArch64::WZR)
    return {};
  if (R == AArch64::SP || R == AArch64::WSP)
    return {hbng::Loc::sp()};
  return {hbng::Loc::x(TRI->getEncodingValue(R))};
}

// Writes the register R from the union of Srcs, or, Bytewise, byte k from
// Srcs[k]: nothing for the zero register. An FP/SIMD register written as B,
// H, S or D has its bytes above cleared (the write zeroes the top of the
// register).
void AArch64HBNGAnnotate::writeReg(hbng::Transfer &T, Register R,
                                   ArrayRef<hbng::Loc> Srcs, bool Bytewise) {
  if (!isFPR(R)) {
    T.assign(locsOf(R), Bytewise ? Srcs.take_front() : Srcs);
    return;
  }
  unsigned N = TRI->getEncodingValue(R), Bytes = regBytes(R);
  hbng::LocList Dsts = hbng::vBytes(N, 0, Bytes);
  if (Bytewise)
    T.assignPer(Dsts, {Srcs}, 1, 1);
  else
    T.assign(Dsts, Srcs);
  T.assign(hbng::vBytes(N, Bytes, 16 - Bytes), {});
}

// Writes the register R per element, as Transfer::assignPer: an FP/SIMD
// register has its bytes above those written cleared.
void AArch64HBNGAnnotate::writeRegPer(hbng::Transfer &T, Register R,
                                      ArrayRef<ArrayRef<hbng::Loc>> Groups,
                                      unsigned In, unsigned Out,
                                      ArrayRef<ArrayRef<hbng::Loc>> Excl) {
  T.assignPer(locsOf(R), Groups, In, Out, Excl);
  if (isFPR(R)) {
    unsigned Bytes = regBytes(R);
    T.assign(hbng::vBytes(TRI->getEncodingValue(R), Bytes, 16 - Bytes), {});
  }
}

// Writes byte k of the register R from Map[k], as Transfer::assignMap; the
// bytes above are cleared as above.
void AArch64HBNGAnnotate::writeRegMap(hbng::Transfer &T, Register R,
                                      ArrayRef<std::optional<hbng::Loc>> Map) {
  T.assignMap(locsOf(R), Map);
  if (isFPR(R)) {
    unsigned Bytes = regBytes(R);
    T.assign(hbng::vBytes(TRI->getEncodingValue(R), Bytes, 16 - Bytes), {});
  }
}

// The value of a register at the instruction's entry. W29 is FP's value too
// (a register offset is read as its W register, extended).
hbng::Term AArch64HBNGAnnotate::termOf(Register R) {
  if (R == AArch64::SP || R == AArch64::WSP)
    return Terms.sp();
  if (R == AArch64::FP || R == AArch64::W29)
    return Terms.fp();
  return Terms.reg(TRI->getEncodingValue(R));
}

// The destination (operand 0) from the union of every register read.
void AArch64HBNGAnnotate::genDataflow(hbng::Transfer &T, MachineInstr &MI) {
  hbng::LocList Srcs;
  for (unsigned I = 1, E = MI.getNumExplicitOperands(); I < E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (MO.isReg() && MO.isUse())
      Srcs.append(locsOf(MO.getReg()));
  }
  writeReg(T, MI.getOperand(0).getReg(), Srcs);
}

// An instruction whose result does not depend on its source, read twice:
// x - x and x ^ x are 0, x & ~x is 0, x | ~x and x ^ ~x are all ones, and
// x - x - !C is 0 or -1 (the flags carry no tag). Only when the second read
// is neither shifted nor extended: x - (x << 1) is a value of x.
static bool cancelsItsSource(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case AArch64::SUBXrs: case AArch64::SUBWrs:
  case AArch64::SUBSXrs: case AArch64::SUBSWrs:
  case AArch64::EORXrs: case AArch64::EORWrs:
  case AArch64::EONXrs: case AArch64::EONWrs:
  case AArch64::BICXrs: case AArch64::BICWrs:
  case AArch64::BICSXrs: case AArch64::BICSWrs:
  case AArch64::ORNXrs: case AArch64::ORNWrs:
    if (AArch64_AM::getShiftValue(MI.getOperand(3).getImm()) != 0)
      return false;
    [[fallthrough]];
  case AArch64::SUBXrr: case AArch64::SUBWrr:
  case AArch64::SUBSXrr: case AArch64::SUBSWrr:
  case AArch64::EORXrr: case AArch64::EORWrr:
  case AArch64::EONXrr: case AArch64::EONWrr:
  case AArch64::BICXrr: case AArch64::BICWrr:
  case AArch64::BICSXrr: case AArch64::BICSWrr:
  case AArch64::ORNXrr: case AArch64::ORNWrr:
  case AArch64::SBCXr: case AArch64::SBCWr:
  case AArch64::SBCSXr: case AArch64::SBCSWr:
    return MI.getOperand(1).getReg() == MI.getOperand(2).getReg();
  default:
    return false;
  }
}

// The destination (operand 0) cleared: a constant, an address from PC, a
// system register (thread pointer, FPCR/FPSR, counters: not data the program
// read or received).
void AArch64HBNGAnnotate::genClear(hbng::Transfer &T, MachineInstr &MI) {
  writeReg(T, MI.getOperand(0).getReg(), {});
}

//===----------------------------------------------------------------------===//
// SIMD/FP registers
//
// A V register has one tag per byte: V0[0:15] is all of v0, V0[4:7] its
// second 4-byte lane. The NEON forms and their statements:
//
//   add  v0.4s, v1.4s, v2.4s      V0[0:15] <- V1[0:15] | V2[0:15] per 4
//     lane i from lane i of each source ("per 4": lanes of 4 bytes)
//   and  v0.16b, v1.16b, v2.16b   V0[0:15] <- V1[0:15] | V2[0:15] per 1
//     byte k from byte k
//   umull v0.4s, v1.4h, v2.4h     V0[0:15] <- V1[0:7] | V2[0:7] per 2:4
//     lanes of 2 bytes in, of 4 bytes out
//   ushll v0.4s, v1.4h, #8        V0[0:15] <- [0 V1[0:1] 0 0 V1[2:3] 0 ...]
//     an extension by whole bytes moves bytes: the ones shifted in and those
//     above the element are 0 (sshll: copies of its top byte)
//   addp v0.4s, v1.4s, v2.4s      V0[0:7] <- V1[0:15] per 8:4
//                                 V0[8:15] <- V2[0:15] per 8:4
//     each lane out from two neighbouring lanes in
//   addv s0, v1.4s                V0[0:3] <- V1[0:15]
//     every byte read into every byte written
//   ext  v0.16b, v1.16b, v2.16b, #3   V0[0:15] <- [V1[3:15] V2[0:2]]
//     byte k from the k-th byte listed
//   cmeq v0.4s, v1.4s, v2.4s      V0[0:15] <- 0 excl V1[0:15] | V2[0:15] per 4
//     clean; "excl" lists what is read but does not reach the result
//   tbl  v0.16b, {v1.16b}, v2.16b V0[0:15] <- V1[0:15] excl V2[0:15] per 1
//     the table, not the index
//   bsl  v0.16b, v1.16b, v2.16b
//       V0[0:15] <- V1[0:15] | V2[0:15] per 1 excl V0[0:15] per 1
//     the data, not the mask (v0)
//   mov  w0, v1.s[1]              X0 <- V1[4:7]
//   movi v0.4s, #1                V0[0:15] <- 0
//
// A 64-bit form (v0.2s) writes V0[0:7] and clears V0[8:15].

// The size of a register in bytes.
unsigned AArch64HBNGAnnotate::regBytes(Register R) {
  return TRI->getRegSizeInBits(*TRI->getMinimalPhysRegClass(R))
             .getFixedValue() /
         8;
}

// A byte map entry for a byte that gets no tag.
static constexpr unsigned ZeroByte = ~0u;

namespace {
// A NEON opcode name split into its mnemonic and arrangement: "ADDv4i32" is
// ADD on 4 elements of 4 bytes, "INSvi32gpr" INS on 4-byte lanes (N = 0).
struct NeonName {
  StringRef Mn;
  unsigned N = 0;
  unsigned E = 0; // bytes
  StringRef Rest;
};

enum NeonFamily {
  NF_None,
  NF_Lane,   // lane-wise, union within each element
  NF_Log,    // exact bytes
  NF_Cmp,    // comparison: clean
  NF_Pair,   // pairwise
  NF_PairL,  // pairwise long, in place
  NF_Red,    // reduction across lanes
  NF_Wide,   // widening, source arrangement in the name
  NF_Narrow, // narrowing, destination arrangement in the name
  NF_Perm,   // permute or lane duplication
  NF_Tbl,    // table lookup
  NF_Sel,    // bitwise select
  NF_Ins,    // lane insert
  NF_Umov,   // lane to a W/X register
  NF_Imm,    // immediate move
};
} // end anonymous namespace

static std::optional<NeonName> parseNeonName(StringRef Name) {
  size_t I = 0;
  while (I < Name.size() && (isUpper(Name[I]) || isDigit(Name[I])))
    ++I;
  NeonName R;
  // A scalar form with an immediate, its (destination) size as a suffix:
  // "SHLd" is SHL on one element of 8 bytes, "SQSHRNs" narrows into 4 bytes.
  if (I > 0 && I + 1 == Name.size() && StringRef("bhsd").contains(Name[I])) {
    R.Mn = Name.take_front(I);
    R.N = 1;
    R.E = Name[I] == 'b' ? 1 : Name[I] == 'h' ? 2 : Name[I] == 's' ? 4 : 8;
    return R;
  }
  // The scalar FP forms of three registers end with their size in bits:
  // "FMULX64" is FMULX on one element of 8 bytes.
  if (I == Name.size() && I > 2 && isUpper(Name[I - 3]) &&
      (Name.ends_with("16") || Name.ends_with("32") || Name.ends_with("64"))) {
    R.Mn = Name.drop_back(2);
    R.N = 1;
    R.E = Name.ends_with("16") ? 2 : Name.ends_with("32") ? 4 : 8;
    return R;
  }
  // The scalar widening multiplies name their sources' size: "SQDMULLi32" is
  // sqdmull d0, s1, s2, taken as one element of 8 bytes.
  unsigned SrcBits = 0;
  if (Name.take_front(I).starts_with("SQDM") && I < Name.size() &&
      Name[I] == 'i' && !Name.drop_front(I + 1).getAsInteger(10, SrcBits) &&
      (SrcBits == 16 || SrcBits == 32)) {
    R.Mn = Name.take_front(I);
    R.N = 1;
    R.E = 2 * SrcBits / 8;
    return R;
  }
  if (I == 0 || I + 1 >= Name.size() || Name[I] != 'v')
    return std::nullopt;
  R.Mn = Name.take_front(I);
  StringRef S = Name.drop_front(I + 1);
  unsigned N = 0;
  if (!S.empty() && isDigit(S[0]) && S.consumeInteger(10, N))
    return std::nullopt;
  if (S.empty())
    return std::nullopt;
  unsigned Bits = 0;
  char T = S[0];
  S = S.drop_front();
  switch (T) {
  case 'i':
  case 'f':
    if (S.consumeInteger(10, Bits))
      return std::nullopt;
    break;
  case 'b': Bits = 8; break;
  case 'h': Bits = 16; break;
  case 's': Bits = 32; break;
  case 'd': Bits = 64; break;
  default:
    return std::nullopt;
  }
  R.N = N;
  R.E = Bits / 8;
  R.Rest = S;
  return R;
}

static NeonFamily getNeonFamily(StringRef Mn) {
  auto Is = [&](std::initializer_list<StringRef> L) {
    return llvm::is_contained(L, Mn);
  };
  if (Is({"ADD", "FADD", "SQADD", "UQADD", "SHADD", "UHADD", "SRHADD",
          "URHADD"}))
    return NF_Lane;
  // The old destination is a source (tied): sqadd of a source of the other
  // signedness into it.
  if (Is({"SUQADD", "USQADD"}))
    return NF_Lane;
  if (Is({"SUB", "FSUB", "SQSUB", "UQSUB", "SHSUB", "UHSUB"}))
    return NF_Lane;
  if (Is({"MUL", "MLA", "MLS", "PMUL", "SQDMULH", "SQRDMULH", "FMUL", "FMULX",
          "FMLA", "FMLS", "FDIV", "SABD", "UABD", "FABD", "SABA", "UABA",
          "SMAX", "SMIN", "UMAX", "UMIN", "FMAX", "FMIN", "FMAXNM", "FMINNM",
          "NEG", "ABS", "SQABS", "SQNEG", "FNEG", "FABS", "FSQRT", "FRECPE",
          "FRSQRTE", "URECPE", "URSQRTE", "FRECPS", "FRSQRTS", "FRECPX",
          "FRINTA", "FRINTI", "FRINTM", "FRINTN", "FRINTP", "FRINTX",
          "FRINTZ"}))
    return NF_Lane;
  if (Is({"SHL", "USHR", "SSHR", "URSHR", "SRSHR", "USRA", "SSRA", "URSRA",
          "SRSRA", "SLI", "SRI", "USHL", "SSHL", "URSHL", "SRSHL", "UQSHL",
          "SQSHL", "SQSHLU", "UQRSHL", "SQRSHL"}))
    return NF_Lane;
  if (Is({"CNT", "CLZ", "CLS", "RBIT"}))
    return NF_Lane;
  if (Is({"FCVTZS", "FCVTZU", "FCVTAS", "FCVTAU", "FCVTMS", "FCVTMU", "FCVTNS",
          "FCVTNU", "FCVTPS", "FCVTPU", "SCVTF", "UCVTF"}))
    return NF_Lane;
  if (Is({"AND", "ORR", "EOR", "BIC", "ORN", "NOT"}))
    return NF_Log;
  if (Is({"CMEQ", "CMGE", "CMGT", "CMHI", "CMHS", "CMLE", "CMLT", "CMTST",
          "FCMEQ", "FCMGE", "FCMGT", "FCMLE", "FCMLT", "FACGE", "FACGT"}))
    return NF_Cmp;
  if (Is({"ADDP", "FADDP", "SMAXP", "SMINP", "UMAXP", "UMINP", "FMAXP",
          "FMINP", "FMAXNMP", "FMINNMP"}))
    return NF_Pair;
  if (Is({"UADDLP", "SADDLP", "UADALP", "SADALP"}))
    return NF_PairL;
  if (Is({"ADDV", "SMAXV", "SMINV", "UMAXV", "UMINV", "SADDLV", "UADDLV",
          "FMAXV", "FMINV", "FMAXNMV", "FMINNMV"}))
    return NF_Red;
  if (Is({"USHLL", "SSHLL", "SHLL"}))
    return NF_Wide;
  if (Is({"UMULL", "SMULL", "PMULL", "UABDL", "SABDL", "SQDMULL"}))
    return NF_Wide;
  if (Is({"UADDL", "SADDL"}))
    return NF_Wide;
  if (Is({"USUBL", "SSUBL"}))
    return NF_Wide;
  if (Is({"UADDW", "SADDW", "USUBW", "SSUBW"}))
    return NF_Wide;
  if (Is({"UMLAL", "SMLAL", "UMLSL", "SMLSL", "SQDMLAL", "SQDMLSL", "UABAL",
          "SABAL"}))
    return NF_Wide;
  if (Is({"FCVTL"}))
    return NF_Wide;
  if (Is({"XTN"}))
    return NF_Narrow;
  if (Is({"SQXTN", "UQXTN", "SQXTUN"}))
    return NF_Narrow;
  if (Is({"SHRN", "RSHRN", "SQSHRN", "UQSHRN", "SQRSHRN", "UQRSHRN", "SQSHRUN",
          "SQRSHRUN"}))
    return NF_Narrow;
  if (Is({"FCVTN", "FCVTXN"}))
    return NF_Narrow;
  if (Is({"ADDHN", "RADDHN", "SUBHN", "RSUBHN"}))
    return NF_Narrow;
  if (Is({"ZIP1", "ZIP2", "UZP1", "UZP2", "TRN1", "TRN2", "EXT", "REV16",
          "REV32", "REV64", "DUP"}))
    return NF_Perm;
  if (Is({"TBL", "TBX"}))
    return NF_Tbl;
  if (Is({"BSL", "BIT", "BIF"}))
    return NF_Sel;
  if (Is({"INS"}))
    return NF_Ins;
  if (Is({"UMOV", "SMOV"}))
    return NF_Umov;
  if (Is({"MOVI", "MVNI", "FMOV"}))
    return NF_Imm;
  return NF_None;
}

// The byte map of a permute: byte k of the destination (DstBytes wide) from
// byte Map[k] of the sources taken one after the other (two-source permutes
// have sources as wide as the destination).
static std::optional<SmallVector<unsigned, 16>>
getPermMap(const NeonName &NN, const MachineInstr &MI, unsigned DstBytes) {
  SmallVector<unsigned, 16> Map(DstBytes);
  unsigned E = NN.E, N = DstBytes / E;
  auto Elem = [&](unsigned Dst, unsigned Src) {
    for (unsigned B = 0; B < E; ++B)
      Map[Dst * E + B] = Src * E + B;
  };
  StringRef Mn = NN.Mn;
  if (Mn == "ZIP1" || Mn == "ZIP2") {
    unsigned H = Mn == "ZIP2" ? N / 2 : 0;
    for (unsigned I = 0; I < N / 2; ++I) {
      Elem(2 * I, H + I);
      Elem(2 * I + 1, N + H + I);
    }
  } else if (Mn == "UZP1" || Mn == "UZP2") {
    for (unsigned I = 0; I < N; ++I)
      Elem(I, 2 * I + (Mn == "UZP2"));
  } else if (Mn == "TRN1" || Mn == "TRN2") {
    for (unsigned I = 0; I < N / 2; ++I) {
      Elem(2 * I, 2 * I + (Mn == "TRN2"));
      Elem(2 * I + 1, N + 2 * I + (Mn == "TRN2"));
    }
  } else if (Mn == "EXT") {
    unsigned Imm = MI.getOperand(3).getImm();
    for (unsigned K = 0; K < DstBytes; ++K)
      Map[K] = Imm + K;
  } else if (Mn.starts_with("REV")) {
    unsigned C = Mn == "REV16" ? 2 : Mn == "REV32" ? 4 : 8; // container bytes
    for (unsigned K = 0; K < DstBytes; ++K) {
      unsigned Base = K / C * C, Ei = K % C / E, B = K % E;
      Map[K] = Base + (C / E - 1 - Ei) * E + B;
    }
  } else if (Mn == "DUP" && NN.Rest == "lane") {
    unsigned Idx = MI.getOperand(2).getImm();
    for (unsigned I = 0; I < N; ++I)
      Elem(I, Idx);
  } else {
    return std::nullopt;
  }
  return Map;
}

// The transfer of a NEON instruction (Advanced SIMD vector and scalar
// forms), std::nullopt if it is not one, or an unsupported one for a NEON
// mnemonic whose form is not described.
std::optional<hbng::Transfer>
AArch64HBNGAnnotate::genNeonAnnotation(MachineInstr &MI) {
  StringRef Name = TII->getName(MI.getOpcode());
  std::optional<NeonName> NN = parseNeonName(Name);
  if (!NN)
    return std::nullopt;
  NeonFamily F = getNeonFamily(NN->Mn);
  if (F == NF_None)
    return std::nullopt;
  hbng::Transfer Unsupported;
  Unsupported.unsupported(Name);
  hbng::Transfer T;

  unsigned NumOps = MI.getNumExplicitOperands();
  MachineOperand &Dst = MI.getOperand(0);
  if (!Dst.isReg() || !Dst.isDef())
    return Unsupported;
  Register Rd = Dst.getReg();
  unsigned RdN = TRI->getEncodingValue(Rd);
  // A destination register that is also read (tied): its old value.
  bool Tied = NumOps > 1 && MI.getOperand(1).isReg() &&
              MI.getOperand(1).isTied() && MI.getOperand(1).isUse();
  bool Indexed = NN->Rest.contains("indexed");
  unsigned E = NN->E;
  unsigned DB = isFPR(Rd) ? regBytes(Rd) : 0;
  // The registers read from operand First on, the by-element register of an
  // indexed form left out.
  auto Srcs = [&](unsigned First) {
    SmallVector<Register, 4> R;
    for (unsigned I = First, Last = Indexed ? NumOps - 2 : NumOps; I < Last;
         ++I)
      if (MI.getOperand(I).isReg() && MI.getOperand(I).isUse())
        R.push_back(MI.getOperand(I).getReg());
    return R;
  };
  // Whether every register of Rs is a V register of Bytes bytes.
  auto AllOf = [&](ArrayRef<Register> Rs, unsigned Bytes) {
    return all_of(Rs, [&](Register R) {
      return isFPR(R) && regBytes(R) == Bytes;
    });
  };
  // Bytes [Lo, Lo + Len) of the V register R.
  auto Bytes = [&](Register R, unsigned Lo, unsigned Len) {
    return hbng::vBytes(TRI->getEncodingValue(R), Lo, Len);
  };
  // Every byte of the V register R.
  auto Whole = [&](Register R) { return Bytes(R, 0, regBytes(R)); };
  // Every byte of every register of Rs.
  auto Union = [&](ArrayRef<Register> Rs) {
    hbng::LocList L;
    for (Register R : Rs)
      L.append(Whole(R));
    return L;
  };
  // One group per register of Rs, for the per forms (Store keeps them).
  auto Groups = [&](ArrayRef<Register> Rs) {
    SmallVector<hbng::LocList, 4> Store;
    for (Register R : Rs)
      Store.push_back(Whole(R));
    return Store;
  };
  auto Views = [&](const SmallVectorImpl<hbng::LocList> &Store) {
    SmallVector<ArrayRef<hbng::Loc>, 4> V;
    for (const hbng::LocList &L : Store)
      V.push_back(L);
    return V;
  };
  // The byte map of Map, entries into the bytes of Rs taken one after the
  // other (ZeroByte: none).
  auto ByteMap = [&](ArrayRef<Register> Rs, ArrayRef<unsigned> Map) {
    hbng::LocList All = Union(Rs);
    SmallVector<std::optional<hbng::Loc>, 16> M;
    for (unsigned I : Map)
      M.push_back(I == ZeroByte ? std::nullopt
                                : std::optional<hbng::Loc>(All[I]));
    return M;
  };

  switch (F) {
  case NF_None:
    break;
  case NF_Lane: {
    // The old destination included when it is read (mla, usra).
    SmallVector<Register, 4> Rs = Srcs(1);
    if (!DB || DB % E || !AllOf(Rs, DB))
      return Unsupported;
    if (!Indexed) {
      SmallVector<hbng::LocList, 4> G = Groups(Rs);
      writeRegPer(T, Rd, Views(G), E, E);
      return T;
    }
    hbng::LocList Lane = Bytes(MI.getOperand(NumOps - 2).getReg(),
                            MI.getOperand(NumOps - 1).getImm() * E, E);
    for (unsigned Lo = 0; Lo < DB; Lo += E) {
      hbng::LocList L;
      for (Register R : Rs)
        L.append(Bytes(R, Lo, E));
      L.append(Lane);
      T.assign(Bytes(Rd, Lo, E), L);
    }
    T.assign(hbng::vBytes(RdN, DB, 16 - DB), {});
    return T;
  }
  case NF_Log: {
    // The old destination included when it is read (orr and bic immediate).
    SmallVector<Register, 4> Rs = Srcs(1);
    if (!DB || !AllOf(Rs, DB))
      return Unsupported;
    SmallVector<hbng::LocList, 4> G = Groups(Rs);
    writeRegPer(T, Rd, Views(G), 1, 1);
    return T;
  }
  case NF_Cmp: {
    SmallVector<Register, 4> Rs = Srcs(1);
    if (!DB || DB % E || !AllOf(Rs, DB))
      return Unsupported;
    SmallVector<hbng::LocList, 4> G = Groups(Rs);
    writeRegPer(T, Rd, {}, E, E, Views(G));
    return T;
  }
  case NF_Pair: {
    SmallVector<Register, 4> Rs = Srcs(1);
    if (NN->Rest == "p") { // scalar pairwise: a reduction of two elements
      writeReg(T, Rd, Union(Rs));
      return T;
    }
    if (!DB || DB % (2 * E) || Rs.size() != 2 || !AllOf(Rs, DB))
      return Unsupported;
    hbng::LocList S0 = Whole(Rs[0]), S1 = Whole(Rs[1]);
    T.assignPer(hbng::vBytes(RdN, 0, DB / 2), {ArrayRef<hbng::Loc>(S0)}, 2 * E,
                E);
    T.assignPer(hbng::vBytes(RdN, DB / 2, DB / 2),
                {ArrayRef<hbng::Loc>(S1)}, 2 * E, E);
    T.assign(hbng::vBytes(RdN, DB, 16 - DB), {});
    return T;
  }
  case NF_PairL: {
    // Output element i, twice as wide, from input elements 2i and 2i+1 of the
    // source ("per <2e>"); the accumulating forms (uadalp) also read the old
    // destination.
    bool Accumulates = NN->Mn == "UADALP" || NN->Mn == "SADALP";
    SmallVector<Register, 4> Rs = Srcs(1);
    if (Tied != Accumulates || !DB || DB != NN->N * E || !AllOf(Rs, DB))
      return Unsupported;
    SmallVector<hbng::LocList, 4> G = Groups(Rs);
    writeRegPer(T, Rd, Views(G), 2 * E, 2 * E);
    return T;
  }
  case NF_Red:
    writeReg(T, Rd, Union(Srcs(1)));
    return T;
  case NF_Wide: {
    // The source arrangement is in the name; the "2" forms read the upper
    // half of their (128-bit) sources. A scalar form (one element of the
    // destination's size) reads elements half as wide: sqdmull d0, s1, s2 or
    // d0, s1, v2.s[1]; pmull v0.1q, v1.1d, v2.1d is not one.
    bool WideFirst = is_contained({"UADDW", "SADDW", "USUBW", "SSUBW"}, NN->Mn);
    bool Accumulates =
        is_contained({"UMLAL", "SMLAL", "UMLSL", "SMLSL", "SQDMLAL", "SQDMLSL",
                      "UABAL", "SABAL"},
                     NN->Mn);
    if (Tied != Accumulates)
      return Unsupported;
    // Rs[0] is the wide source (uaddw) or the old destination (umlal, tied);
    // the by-element register of an indexed form is not in Rs.
    SmallVector<Register, 4> Rs = Srcs(1);
    bool WideRs0 = WideFirst || Accumulates;
    ArrayRef<Register> Narrow = ArrayRef<Register>(Rs).drop_front(WideRs0);
    bool Scalar = NN->N == 1 && DB == E;
    unsigned SE = Scalar ? E / 2 : E; // a source element's size
    // The element an indexed form reads in every lane.
    hbng::LocList Elem;
    if (Indexed)
      Elem = Bytes(MI.getOperand(NumOps - 2).getReg(),
                   MI.getOperand(NumOps - 1).getImm() * SE, SE);
    if (Scalar) {
      if (!SE || Narrow.empty() || !AllOf(Narrow, SE) ||
          (WideRs0 && !AllOf(Rs[0], E)))
        return Unsupported;
      hbng::LocList L;
      if (WideRs0)
        L.append(Whole(Rs[0]));
      L.append(Union(Narrow));
      L.append(Elem);
      writeReg(T, Rd, L);
      return T;
    }
    unsigned SB = NN->N * E;
    if ((SB != 8 && SB != 16) || DB != 16)
      return Unsupported;
    // The wide, accumulating and by-element forms, lane by lane, with the
    // narrow sources widened.
    if (WideRs0 || Indexed) {
      if (Narrow.empty() || !AllOf(Narrow, SB) || (WideRs0 && !AllOf(Rs[0], 16)))
        return Unsupported;
      for (unsigned Lo = 0; Lo < 16; Lo += 2 * E) {
        hbng::LocList L;
        if (WideRs0)
          L.append(Bytes(Rs[0], Lo, 2 * E));
        for (Register R : Narrow)
          L.append(Bytes(R, SB - 8 + Lo / 2, E));
        L.append(Elem);
        T.assign(Bytes(Rd, Lo, 2 * E), L);
      }
      return T;
    }
    if (!AllOf(Rs, SB))
      return Unsupported;
    // An extension by whole bytes (shll shifts by the element size): byte J
    // of lane I from byte J - S of element I, 0 below, and above 0 or, signed,
    // the element's top byte.
    bool Signed = NN->Mn == "SSHLL";
    if (Signed || NN->Mn == "USHLL" || NN->Mn == "SHLL") {
      unsigned Shift = NN->Mn == "SHLL" ? 8 * E : MI.getOperand(2).getImm();
      if (Shift % 8 == 0) {
        unsigned S = Shift / 8, Lo = SB - 8;
        SmallVector<unsigned, 16> Map;
        for (unsigned I = 0; I < 8 / E; ++I)
          for (unsigned J = 0; J < 2 * E; ++J)
            Map.push_back(J < S          ? ZeroByte
                          : J - S < E    ? Lo + I * E + J - S
                          : Signed       ? Lo + I * E + E - 1
                                         : ZeroByte);
        writeRegMap(T, Rd, ByteMap(Rs, Map));
        return T;
      }
    }
    SmallVector<hbng::LocList, 2> G;
    for (Register R : Rs)
      G.push_back(Bytes(R, SB - 8, 8));
    writeRegPer(T, Rd, Views(G), E, 2 * E);
    return T;
  }
  case NF_Narrow: {
    // The destination arrangement is in the name, after the sources' for the
    // high-half forms (ADDHNv4i32_v4i16: addhn v0.4h, v1.4s, v2.4s); the "2"
    // forms write the upper half of the destination and keep the lower one.
    unsigned N = NN->N, El = E;
    // The scalar fcvtxn names its source: FCVTXNv1i64 is fcvtxn s0, d1.
    if (NN->Mn == "FCVTXN" && N == 1)
      El = E / 2;
    if (NN->Rest.starts_with("_v")) {
      std::string D = ("X" + NN->Rest.drop_front()).str();
      std::optional<NeonName> DN = parseNeonName(D);
      if (!DN || !DN->Rest.empty())
        return Unsupported;
      N = DN->N;
      El = DN->E;
    }
    unsigned Out = N * El;
    bool Upper = Out == 16;
    if (!N || Upper != Tied || !DB)
      return Unsupported;
    SmallVector<Register, 4> Rs = Srcs(Tied ? 2 : 1);
    if (!AllOf(Rs, Upper ? 16 : 2 * Out))
      return Unsupported;
    SmallVector<hbng::LocList, 4> G = Groups(Rs);
    if (Upper) {
      T.assignPer(hbng::vBytes(RdN, 8, 8), Views(G), 2 * El, El);
      return T;
    }
    if (DB != Out)
      return Unsupported;
    writeRegPer(T, Rd, Views(G), 2 * El, El);
    return T;
  }
  case NF_Perm: {
    if (NN->Mn == "DUP" && NN->Rest == "gpr") { // X -> V, every element
      writeReg(T, Rd, locsOf(MI.getOperand(1).getReg()));
      return T;
    }
    if (!NN->N || !DB)
      return Unsupported;
    unsigned NumSrc = NN->Mn == "DUP" || NN->Mn.starts_with("REV") ? 1 : 2;
    SmallVector<Register, 2> Rs;
    for (unsigned I = 1; I <= NumSrc; ++I)
      Rs.push_back(MI.getOperand(I).getReg());
    // The map of a two-source permute counts in sources as wide as the
    // destination.
    if (NumSrc == 2 && !AllOf(Rs, DB))
      return Unsupported;
    std::optional<SmallVector<unsigned, 16>> Map = getPermMap(*NN, MI, DB);
    if (!Map)
      return Unsupported;
    writeRegMap(T, Rd, ByteMap(Rs, *Map));
    return T;
  }
  case NF_Tbl: {
    // (Vd, table list, Vm), or (Vd, Vd, table list, Vm) for TBX, which keeps
    // the old byte for an index out of range.
    bool Tbx = NN->Mn == "TBX";
    if (Tbx != Tied)
      return Unsupported;
    Register List = MI.getOperand(Tbx ? 2 : 1).getReg();
    SmallVector<Register, 4> Table;
    if (AArch64::FPR128RegClass.contains(List)) {
      Table.push_back(List);
    } else {
      static const unsigned QSub[] = {AArch64::qsub0, AArch64::qsub1,
                                      AArch64::qsub2, AArch64::qsub3};
      for (unsigned Sub : QSub)
        if (Register R = TRI->getSubReg(List, Sub))
          Table.push_back(R);
    }
    Register Idx = MI.getOperand(Tbx ? 3 : 2).getReg();
    if (!DB || !AllOf(Idx, DB))
      return Unsupported;
    // Byte k from the table (and, for TBX, the old byte k), the index
    // byte k read only.
    for (unsigned K = 0; K < DB; ++K) {
      hbng::LocList L = Union(Table);
      if (Tbx)
        L.append(Bytes(Rd, K, 1));
      T.assign(Bytes(Rd, K, 1), L, Bytes(Idx, K, 1));
    }
    T.assign(hbng::vBytes(RdN, DB, 16 - DB), {});
    return T;
  }
  case NF_Sel: {
    // (Vd, Vd tied, Vn, Vm). BSL: Vd is the mask, Vn and Vm the data.
    // BIT/BIF: Vm is the mask, Vd's old bits and Vn the data.
    if (!Tied || !DB)
      return Unsupported;
    Register Old = MI.getOperand(1).getReg(), Rn = MI.getOperand(2).getReg(),
             Rm = MI.getOperand(3).getReg();
    bool BSL = NN->Mn == "BSL";
    SmallVector<Register, 2> Data = {BSL ? Rn : Old, BSL ? Rm : Rn};
    Register Mask = BSL ? Old : Rm;
    SmallVector<hbng::LocList, 4> G = Groups(Data);
    hbng::LocList M = Whole(Mask);
    writeRegPer(T, Rd, Views(G), 1, 1, {ArrayRef<hbng::Loc>(M)});
    return T;
  }
  case NF_Ins: {
    // (Vd, Vd tied, idx, Rn [, idx2]): one lane from Rn, the others kept.
    if (!Tied || NN->N != 0)
      return Unsupported;
    hbng::LocList D = Bytes(Rd, MI.getOperand(2).getImm() * E, E);
    Register Rn = MI.getOperand(3).getReg();
    if (NN->Rest == "gpr") {
      T.assign(D, locsOf(Rn));
      return T;
    }
    if (NN->Rest == "lane") {
      hbng::LocList S = Bytes(Rn, MI.getOperand(4).getImm() * E, E);
      T.assignPer(D, {ArrayRef<hbng::Loc>(S)}, 1, 1);
      return T;
    }
    return Unsupported;
  }
  case NF_Umov:
    // (Rd, Vn, idx): one lane to a W/X register.
    if (NN->N != 0)
      return Unsupported;
    writeReg(T, Rd,
             Bytes(MI.getOperand(1).getReg(), MI.getOperand(2).getImm() * E,
                   E));
    return T;
  case NF_Imm:
    // (Vd, imm8 [, shift]); the ORR/BIC immediate forms are NF_Log.
    if (Tied || NumOps < 2 || !MI.getOperand(1).isImm())
      return Unsupported;
    writeReg(T, Rd, {});
    return T;
  }
  return Unsupported;
}

// The value of a tracked register (SP, FP) computed from another one:
//   <Dst> <- <Src> + b        (b in bytes, may be negative)
void AArch64HBNGAnnotate::setTrackedValue(hbng::Transfer &T, Register Dst,
                                          Register Src, int64_t Imm) {
  (Dst == AArch64::SP ? T.SP : T.FP) = Terms.plus(termOf(Src), Imm);
}

// The value of SP written by MI from sources that are not all tracked (see
// generateAnnotation for the forms).
void AArch64HBNGAnnotate::genSPWrite(hbng::Transfer &T, MachineInstr &MI,
                                     const HBNGSPWrite &W) {
  std::string Unsupported = "SP <- " + TII->getName(MI.getOpcode()).str();
  if (!W.Supported)
    return T.unsupported(Unsupported);
  hbng::Term Rn = termOf(MI.getOperand(W.Rn).getReg());
  int64_t Imm = MI.getOperand(2).isImm() ? MI.getOperand(2).getImm() : 0;
  switch (MI.getOpcode()) {
  case AArch64::ADDXri:
  case AArch64::SUBXri: {
    int64_t Bytes = Imm << AArch64_AM::getShiftValue(MI.getOperand(3).getImm());
    T.SP = Terms.plus(Rn, W.IsSub ? -Bytes : Bytes);
    return;
  }
  case AArch64::ANDXri:
    T.SP = Terms.mask(Rn, AArch64_AM::decodeLogicalImmediate(Imm, 64));
    return;
  case AArch64::ORRXri:
  case AArch64::EORXri:
    return T.unsupported(Unsupported); // no such term
  default: { // ADD/SUB extended register
    unsigned Ext = MI.getOperand(3).getImm();
    hbng::Ext Kind;
    switch (AArch64_AM::getArithExtendType(Ext)) {
    case AArch64_AM::UXTW: Kind = hbng::Ext::UXTW; break;
    case AArch64_AM::SXTW: Kind = hbng::Ext::SXTW; break;
    case AArch64_AM::UXTX: Kind = hbng::Ext::UXTX; break;
    case AArch64_AM::SXTX: Kind = hbng::Ext::SXTX; break;
    default:
      return T.unsupported(Unsupported); // a byte or halfword extension
    }
    hbng::Term Rm = Terms.ext(termOf(MI.getOperand(W.Rm).getReg()), Kind,
                              AArch64_AM::getArithShiftValue(Ext));
    T.SP = W.IsSub ? Terms.sub(Rn, Rm) : Terms.add(Rn, Rm);
    return;
  }
  }
}

// The address of a load or store: a term (the base's value, and a register
// offset or LO12 added to it) and an offset in bytes. The address of a
// post-indexed access is the base before the write-back.
std::pair<hbng::Term, int64_t>
AArch64HBNGAnnotate::genAddress(MachineInstr &MI, const HBNGMemAccess &A) {
  hbng::Term Base = termOf(MI.getOperand(A.Base).getReg());
  if (A.Offset < 0 || A.Mode == HBNGMemAccess::PostIndex)
    return {Base, 0};
  if (A.isRegOffset()) {
    Register Rm = MI.getOperand(A.Offset).getReg();
    if (Rm == AArch64::XZR || Rm == AArch64::WZR)
      return {Base, 0}; // adds 0
    bool Signed = MI.getOperand(A.Offset + 1).getImm();
    bool DoShift = MI.getOperand(A.Offset + 2).getImm();
    hbng::Ext Kind = A.Mode == HBNGMemAccess::RegOffsetW
                         ? (Signed ? hbng::Ext::SXTW : hbng::Ext::UXTW)
                         : (Signed ? hbng::Ext::SXTX : hbng::Ext::UXTX);
    return {Terms.add(Base, Terms.ext(termOf(Rm), Kind,
                                      DoShift ? Log2_32(A.Size) : 0)),
            0};
  }
  const MachineOperand &Off = MI.getOperand(A.Offset);
  // A symbol's low 12 bits (":lo12:sym", ":got_lo12:sym"): the linker writes
  // the offset into the instruction, LO12 names it.
  if (!Off.isImm())
    return {Terms.add(Base, Terms.lo12()), 0};
  return {Base, Off.getImm() * (A.isScaledImm() ? int64_t(A.Size) : 1)};
}

// Annotations of a load or store: one line per data register, then the
// write-back of a tracked base, then the store-exclusive status:
//   Rt <- M[<Rn> + off .. +size]                  (load)
//   M[<Rn> + off .. +size] <- Rt                  (store)
//   M[<Rn> + off + i] <- M[<Rn> + off + i] | Rt   (store-exclusive, per byte)
//   <Rn> <- <Rn> + b                              (write-back of SP or FP)
//   Ws <- 0                                       (store-exclusive status)
// An FP/SIMD register moves byte for byte ("per 1"), a general register has
// one tag for all the bytes. No pointer tainting: the tags of the address
// registers do not reach the data. The write-back leaves the tag of the base
// unchanged (an immediate is added), so it needs a line only for a tracked
// base, whose value it updates; any other base is sent at its next access.
hbng::Transfer AArch64HBNGAnnotate::genMemAnnotation(MachineInstr &MI,
                                                     const HBNGMemAccess &A) {
  if (A.Vec != HBNGMemAccess::NotVec)
    return genVecMemAnnotation(MI, A);
  hbng::Transfer T;
  auto [Addr, Off] = genAddress(MI, A);
  for (unsigned I = 0; I < A.NumData; ++I) {
    Register Rt = getHBNGDataReg(MI, A, I, TRI);
    hbng::LocList Mem =
        hbng::memBytes(Terms, Terms.plus(Addr, Off + I * A.Size), A.Size);
    if (!A.IsStore) {
      writeReg(T, Rt, Mem, isFPR(Rt));
      continue;
    }
    hbng::LocList Src = locsOf(Rt);
    if (A.Status < 0) {
      if (isFPR(Rt))
        T.assignPer(Mem, {Src}, 1, 1);
      else
        T.assign(Mem, Src);
      continue;
    }
    // A store-exclusive may fail and leave memory as it was: each byte keeps
    // its tag or takes the register's (the same sources for all the bytes
    // would give every byte the tags of all of them). Storing the zero
    // register keeps the bytes.
    if (Src.empty())
      continue;
    for (const hbng::Loc &Byte : Mem) {
      hbng::LocList Srcs = Src;
      Srcs.push_back(Byte);
      T.assign({Byte}, Srcs);
    }
  }

  Register Base = MI.getOperand(A.Base).getReg();
  if (isHBNGTrackedReg(Base))
    if (std::optional<int64_t> Bytes = getHBNGWritebackBytes(MI, A))
      setTrackedValue(T, Base, Base, *Bytes);

  if (A.Status >= 0)
    writeReg(T, MI.getOperand(A.Status).getReg(), {});
  return T;
}

// Annotations of a structured (NEON) load or store: one line per data
// register, byte for byte, then the write-back of a tracked base (see
// HBNGMemAccess for the forms):
//   V0[0:15] <- M[<X0> .. +16] per 1                     ld1 {v0.16b}
//   V1[0:15] <- M[<X0> + 1 .. +1 step 4 times 16] per 1  ld4, register 1
//   V0[0:15] <- M[<X0> .. +4 step 0 times 4] per 1       ld1r {v0.4s}
//   V0[8:11] <- M[<X0> .. +4] per 1                      ld1 {v0.s}[2]
// A load of a 64-bit register clears its upper 8 bytes, a load of a lane
// keeps the other bytes. The write-back by a register is not described.
hbng::Transfer
AArch64HBNGAnnotate::genVecMemAnnotation(MachineInstr &MI,
                                         const HBNGMemAccess &A) {
  hbng::Transfer T;
  auto [Addr, Off] = genAddress(MI, A);
  unsigned E = A.ElemSize, N = A.RegBytes / E;
  for (unsigned I = 0; I < A.NumData; ++I) {
    Register Rt = getHBNGDataReg(MI, A, I, TRI);
    unsigned V = TRI->getEncodingValue(Rt);
    hbng::LocList Mem, Reg = hbng::vBytes(V, 0, A.RegBytes);
    switch (A.Vec) {
    case HBNGMemAccess::NotVec:
      llvm_unreachable("HBNG: not a structured access");
    case HBNGMemAccess::VecMultiple:
      Mem = hbng::memBytes(Terms, Terms.plus(Addr, Off + I * A.RegBytes),
                           A.RegBytes);
      break;
    case HBNGMemAccess::VecInterleaved:
      Mem = hbng::memBytes(Terms, Terms.plus(Addr, Off + I * E), E,
                           A.NumData * E, N);
      break;
    case HBNGMemAccess::VecReplicate:
      Mem = hbng::memBytes(Terms, Terms.plus(Addr, Off + I * E), E, 0, N);
      break;
    case HBNGMemAccess::VecLane:
      Mem = hbng::memBytes(Terms, Terms.plus(Addr, Off + I * E), E);
      Reg = hbng::vBytes(V, MI.getOperand(A.Lane).getImm() * E, E);
      break;
    }
    if (A.IsStore)
      T.assignPer(Mem, {Reg}, 1, 1);
    else if (A.Vec == HBNGMemAccess::VecLane)
      T.assignPer(Reg, {Mem}, 1, 1);
    else
      writeReg(T, Rt, Mem, true);
  }

  Register Base = MI.getOperand(A.Base).getReg();
  if (isHBNGTrackedReg(Base))
    if (std::optional<int64_t> Bytes = getHBNGWritebackBytes(MI, A))
      setTrackedValue(T, Base, Base, *Bytes);
  return T;
}

//===----------------------------------------------------------------------===//
// Main annotation generation function

// A send: one line per FIFO entry, in the order the STM emits them. A pair
// "stp a, b, [x18]" stores a at the lower address and b at the higher one,
// and the STM emits the higher address first: "send b", then "send a".
// FP is never sent as an address (it is tracked): a send of x29 follows a
// write of FP whose value the annotations cannot compute, and gives it.
// A send pseudo sends its register, or a pair its two, in its order.
hbng::Transfer AArch64HBNGAnnotate::genSends(MachineInstr &MI) {
  SmallVector<Register, 2> Regs;
  if (isHBNGSendPseudo(MI)) {
    Regs = getHBNGSendPseudoRegs(MI);
  } else {
    std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI);
    for (unsigned I = A->NumData; I-- > 0;)
      Regs.push_back(getHBNGDataReg(MI, *A, I, TRI));
  }
  hbng::Transfer T;
  for (Register R : Regs) {
    T.Sends.push_back(TRI->getEncodingValue(R));
    if (R == AArch64::FP)
      T.FP = Terms.reg(29);
  }
  return T;
}

// Inline asm, one group per instruction: svc for a system call; any other
// instruction is not described, so it is reported like an unsupported opcode
// rather than dropped. An empty statement (a compiler barrier) has no group.
// A system register access and a barrier are described as their opcodes are:
// msr and barriers annotate nothing, and mrs gives a clean value when the
// statement is the whole asm and has one output register (otherwise which
// output it writes is not known here).
std::vector<hbng::Transfer>
AArch64HBNGAnnotate::genInlineAsmGroups(MachineInstr &MI) {
  SmallVector<std::string, 4> Mnemonics = getInlineAsmMnemonics(MI);
  SmallVector<Register, 2> Outs;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isDef() && !MO.isImplicit()) // not a clobber
      Outs.push_back(MO.getReg());
  std::vector<hbng::Transfer> Groups(Mnemonics.size());
  for (auto [Mnemonic, T] : zip(Mnemonics, Groups)) {
    if (Mnemonic == "svc")
      T.Opaque = hbng::Transfer::Svc;
    else if (Mnemonic == "mrs" && Mnemonics.size() == 1 && Outs.size() == 1)
      writeReg(T, Outs[0], {});
    else if (!is_contained({"msr", "dmb", "dsb", "isb", "nop"}, Mnemonic))
      T.unsupported("INLINEASM " + Mnemonic);
  }
  return Groups;
}

// Adds to Into what From writes, From writing none of the same locations.
static void merge(hbng::Transfer &Into, hbng::Transfer &&From) {
  Into.Deps.insert(From.Deps.begin(), From.Deps.end());
  if (From.SP)
    Into.SP = From.SP;
  if (From.FP)
    Into.FP = From.FP;
  Into.Sends.append(From.Sends.begin(), From.Sends.end());
}

std::vector<hbng::Transfer>
AArch64HBNGAnnotate::generateGroups(MachineInstr &MI) {
  if (MI.isMetaInstruction() || MI.getOpcode() == AArch64::HBNG_ATOM_LABEL)
    return {};
  if (MI.isInlineAsm()) {
    std::vector<hbng::Transfer> Groups = genInlineAsmGroups(MI);
    // The tracked registers it writes are known once the statement is done.
    hbng::Transfer Values = genTrackedValues(MI);
    if (!Groups.empty() && !Values.empty()) {
      if (Values.isOpaque())
        Groups.back() = std::move(Values);
      else if (Groups.back().Opaque != hbng::Transfer::Unsupported)
        merge(Groups.back(), std::move(Values));
    }
    return Groups;
  }
  // The sends are not part of the program: the coprocessor receives them.
  if (isHBNGSend(MI))
    return {genSends(MI)};
  switch (MI.getOpcode()) {
  // The jump-table dispatch, operands (dst, scratch, table, index), is
  // emitted as three instructions, one group each:
  //   adr  dst, label                   dst <- 0
  //   ldrb scratch, [table, index]      scratch <- 0
  //   add  dst, dst, scratch, lsl #2    dst <- dst | scratch
  // (ldrh or ldrsw for wider entries). The label's address is a constant
  // computed from PC. The table is read-only and holds constants, so the
  // entry loaded is clean too: the index only selects which entry is read,
  // and, as for any load, the address's tag does not reach the data.
  case AArch64::JumpTableDest32:
  case AArch64::JumpTableDest16:
  case AArch64::JumpTableDest8: {
    Register Dst = MI.getOperand(0).getReg();
    Register Scratch = MI.getOperand(1).getReg();
    std::vector<hbng::Transfer> Groups(3);
    writeReg(Groups[0], Dst, {});
    writeReg(Groups[1], Scratch, {});
    writeReg(Groups[2], Dst, {locsOf(Dst)[0], locsOf(Scratch)[0]});
    return Groups;
  }
  // No instruction (their size is the default of 4): .cfi_b_key_frame, and
  // a register left undefined.
  case AArch64::EMITBKEY:
  case AArch64::INIT_UNDEF:
    return {};
  // The barrier after a ret or br of -mharden-sls (never executed): dsb sy and
  // isb, or sb, writing nothing.
  case AArch64::SpeculationBarrierISBDSBEndBB:
    return {{}, {}};
  case AArch64::SpeculationBarrierSBEndBB:
    return {{}};
  default:
    break;
  }
  unsigned Size = TII->getInstSizeInBytes(MI);
  if (Size == 0)
    return {};
  if (Size == 4)
    return {generateAnnotation(MI)};
  // A pseudo expanded at emission into several instructions: its size is an
  // upper bound, so the groups may not line up with the code.
  std::vector<hbng::Transfer> Groups(Size / 4);
  Groups[0].unsupported(TII->getName(MI.getOpcode()));
  return Groups;
}

// The annotation of MI: the dependencies it writes (generateTagAnnotation),
// then the values of the tracked registers it writes (SP, FP, see
// isHBNGTrackedReg):
//   <Dst> <- <Src> + b                immediate update from a tracked register
//   <SP> <- <Rn> + b                  any other write of SP, from sent sources
//   <SP> <- <Rn> +|- ext(<Rm>, e, s)  (sent before MI unless tracked)
//   <SP> <- (<Rn> & mask)
// with b in bytes (negative for SUB). Any other write of FP sends its value
// right after MI, and that send's group gives it (genSends). The
// write-back of an access based on a tracked register is described with the
// access (genMemAnnotation). An unsupported statement replaces everything
// else.
hbng::Transfer AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI) {
  hbng::Transfer T = generateTagAnnotation(MI);
  hbng::Transfer Values = genTrackedValues(MI);
  if (T.Opaque == hbng::Transfer::Unsupported)
    return T;
  if (Values.isOpaque())
    return std::move(Values);
  merge(T, std::move(Values));
  return T;
}

// The values of the tracked registers MI writes (see generateAnnotation).
hbng::Transfer AArch64HBNGAnnotate::genTrackedValues(MachineInstr &MI) {
  hbng::Transfer T;
  if (std::optional<HBNGTrackedUpdate> U = getHBNGTrackedImmUpdate(MI))
    setTrackedValue(T, U->Dst, U->Src, U->Imm);
  if (std::optional<HBNGSPWrite> W = getHBNGSPWrite(MI, TRI))
    genSPWrite(T, MI, *W);
  return T;
}

hbng::Transfer AArch64HBNGAnnotate::generateTagAnnotation(MachineInstr &MI) {
  hbng::Transfer T;
  Register Rd = MI.getNumOperands() && MI.getOperand(0).isReg()
                    ? MI.getOperand(0).getReg()
                    : Register();

  if (TII->isGPRZero(MI) || cancelsItsSource(MI)) {
    genClear(T, MI);
    return T;
  }

  if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI))
    return genMemAnnotation(MI, *A);

  if (std::optional<hbng::Transfer> V = genNeonAnnotation(MI))
    return *V;

  switch (MI.getOpcode()) {
    //=== ARITHMETIC ===//
    // One Register or Immediate (rounding to an integral value, square root:
    // every byte of the scalar from every byte of its source)
    case AArch64::FNEGSr: case AArch64::FNEGDr:
    case AArch64::FABSSr: case AArch64::FABSDr:
    case AArch64::FSQRTSr: case AArch64::FSQRTDr:
    case AArch64::FRINTASr: case AArch64::FRINTADr:
    case AArch64::FRINTISr: case AArch64::FRINTIDr:
    case AArch64::FRINTMSr: case AArch64::FRINTMDr:
    case AArch64::FRINTNSr: case AArch64::FRINTNDr:
    case AArch64::FRINTPSr: case AArch64::FRINTPDr:
    case AArch64::FRINTXSr: case AArch64::FRINTXDr:
    case AArch64::FRINTZSr: case AArch64::FRINTZDr:
      genDataflow(T, MI);
      break;
    // Two registers
    case AArch64::ADDXrr: case AArch64::ADDXrx: case AArch64::ADDXrs:
    case AArch64::ADDWrr: case AArch64::ADDWrx: case AArch64::ADDWrs:
    case AArch64::ADDXrx64:
    case AArch64::FADDSrr: case AArch64::FADDDrr:
      genDataflow(T, MI);
      break;
    case AArch64::SUBXrr: case AArch64::SUBXrx: case AArch64::SUBXrs:
    case AArch64::SUBWrr: case AArch64::SUBWrx: case AArch64::SUBWrs:
    case AArch64::SUBXrx64:
    case AArch64::FSUBDrr: case AArch64::FSUBSrr:
      genDataflow(T, MI);
      break;
    case AArch64::ADDSXrr: case AArch64::ADDSXrx: case AArch64::ADDSXrs:
    case AArch64::ADDSWrr: case AArch64::ADDSWrx: case AArch64::ADDSWrs:
    case AArch64::ADDSXrx64:
      genDataflow(T, MI);
      break;
    case AArch64::SUBSXrr: case AArch64::SUBSXrx: case AArch64::SUBSXrs:
    case AArch64::SUBSWrr: case AArch64::SUBSWrx: case AArch64::SUBSWrs:
    case AArch64::SUBSXrx64:
      genDataflow(T, MI);
      break;
    case AArch64::ADCSXr: case AArch64::ADCSWr:
    case AArch64::SBCSXr: case AArch64::SBCSWr:
      genDataflow(T, MI);
      break;
    case AArch64::ADCXr: case AArch64::ADCWr:
    case AArch64::SBCXr: case AArch64::SBCWr:
      genDataflow(T, MI);
      break;
    case AArch64::UMULHrr: case AArch64::SMULHrr:
    case AArch64::FMULSrr: case AArch64::FMULDrr:
    case AArch64::FNMULSrr: case AArch64::FNMULDrr:
      genDataflow(T, MI);
      break;
    // Register and Immediate
    case AArch64::ADDXri: case AArch64::ADDWri:
      genDataflow(T, MI);
      break;
    case AArch64::SUBXri: case AArch64::SUBWri:
      genDataflow(T, MI);
      break;
    case AArch64::ADDSXri: case AArch64::ADDSWri:
    case AArch64::SUBSXri: case AArch64::SUBSWri:
      genDataflow(T, MI);
      break;
    case AArch64::UDIVWr: case AArch64::UDIVXr:
    case AArch64::SDIVWr: case AArch64::SDIVXr:
    case AArch64::FDIVSrr: case AArch64::FDIVDrr:
      genDataflow(T, MI);
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
      genDataflow(T, MI);
      break;

    //=== FP MINIMUM AND MAXIMUM ===//
    // The result is one of the data operands, not a comparison result.
    case AArch64::FMAXNMSrr: case AArch64::FMAXNMDrr:
    case AArch64::FMINNMSrr: case AArch64::FMINNMDrr:
    case AArch64::FMAXSrr: case AArch64::FMAXDrr:
    case AArch64::FMINSrr: case AArch64::FMINDrr:
      genDataflow(T, MI);
      break;

    //=== FP CONVERSION ===//
    case AArch64::FCVTDSr: case AArch64::FCVTSDr:
    case AArch64::FCVTHSr: case AArch64::FCVTSHr:
    case AArch64::FCVTHDr: case AArch64::FCVTDHr:
// FP to integer (every rounding, W and X destinations) and back.
#define HBNG_FCVT_INT(R)                                                       \
  case AArch64::FCVT##R##UWHr: case AArch64::FCVT##R##UWSr:                    \
  case AArch64::FCVT##R##UWDr: case AArch64::FCVT##R##UXHr:                    \
  case AArch64::FCVT##R##UXSr: case AArch64::FCVT##R##UXDr:
    HBNG_FCVT_INT(AS) HBNG_FCVT_INT(AU) HBNG_FCVT_INT(MS) HBNG_FCVT_INT(MU)
    HBNG_FCVT_INT(NS) HBNG_FCVT_INT(NU) HBNG_FCVT_INT(PS) HBNG_FCVT_INT(PU)
    HBNG_FCVT_INT(ZS) HBNG_FCVT_INT(ZU)
#undef HBNG_FCVT_INT
    // The fixed-point forms (a number of fraction bits)
    case AArch64::FCVTZSSWHri: case AArch64::FCVTZSSWSri: case AArch64::FCVTZSSWDri:
    case AArch64::FCVTZSSXHri: case AArch64::FCVTZSSXSri: case AArch64::FCVTZSSXDri:
    case AArch64::FCVTZUSWHri: case AArch64::FCVTZUSWSri: case AArch64::FCVTZUSWDri:
    case AArch64::FCVTZUSXHri: case AArch64::FCVTZUSXSri: case AArch64::FCVTZUSXDri:
    case AArch64::SCVTFSWHri: case AArch64::SCVTFSWSri: case AArch64::SCVTFSWDri:
    case AArch64::SCVTFSXHri: case AArch64::SCVTFSXSri: case AArch64::SCVTFSXDri:
    case AArch64::UCVTFSWHri: case AArch64::UCVTFSWSri: case AArch64::UCVTFSWDri:
    case AArch64::UCVTFSXHri: case AArch64::UCVTFSXSri: case AArch64::UCVTFSXDri:
    case AArch64::SCVTFUWHri: case AArch64::SCVTFUWSri: case AArch64::SCVTFUWDri:
    case AArch64::SCVTFUXHri: case AArch64::SCVTFUXSri: case AArch64::SCVTFUXDri:
    case AArch64::UCVTFUWHri: case AArch64::UCVTFUWSri: case AArch64::UCVTFUWDri:
    case AArch64::UCVTFUXHri: case AArch64::UCVTFUXSri: case AArch64::UCVTFUXDri:
      genDataflow(T, MI);
      break;

    //=== ADDR COMPUTATION ===//
    case AArch64::ADR:
      genClear(T, MI);
      break;
    case AArch64::ADRP:
      genClear(T, MI);
      break;
    //=== LOGICAL ===//
    // reg
    case AArch64::ANDXrr: case AArch64::ANDXrs: case AArch64::ANDWrr: case AArch64::ANDWrs:
    case AArch64::BICXrr: case AArch64::BICXrs: case AArch64::BICWrr: case AArch64::BICWrs:
    case AArch64::EORXrr: case AArch64::EORXrs: case AArch64::EORWrr: case AArch64::EORWrs:
    case AArch64::EONXrr: case AArch64::EONXrs: case AArch64::EONWrr: case AArch64::EONWrs:
    case AArch64::ORNXrr: case AArch64::ORNXrs: case AArch64::ORNWrr: case AArch64::ORNWrs:
    case AArch64::ORRXrr: case AArch64::ORRXrs: case AArch64::ORRWrr: case AArch64::ORRWrs:
    // imm
    case AArch64::EORWri: case AArch64::EORXri:
    case AArch64::ORRXri: case AArch64::ORRWri:
    case AArch64::ANDXri: case AArch64::ANDWri:
      genDataflow(T, MI);
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs: case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs: case AArch64::BICSWrr: case AArch64::BICSWrs:
    case AArch64::ANDSXri: case AArch64::ANDSWri:
      genDataflow(T, MI);
      break;
    //=== SHIFTS ===//
    case AArch64::ASRVWr: case AArch64::ASRVXr:
    case AArch64::LSRVWr: case AArch64::LSRVXr:
    case AArch64::LSLVWr: case AArch64::LSLVXr:
    case AArch64::RORVWr: case AArch64::RORVXr:
      genDataflow(T, MI);
      break;
    //=== MOVES ===//
    // Partial move
    case AArch64::MOVKWi: case AArch64::MOVKXi:
      genDataflow(T, MI);
      break;
    // Total move
    case AArch64::MOVNWi: case AArch64::MOVNXi:
    case AArch64::MOVZXi: case AArch64::MOVZWi:
    case AArch64::MOVID: case AArch64::FMOVSi: case AArch64::FMOVDi:
    case AArch64::FMOVHi:
      genDataflow(T, MI);
      break;
    // Zero
    case AArch64::FMOVS0: case AArch64::FMOVD0: case AArch64::FMOVH0:
      genClear(T, MI);
      break;
    // Register move: between FP/SIMD registers byte for byte ("per 1"), the
    // bytes above the scalar cleared; to or from a W/X register, its one tag.
    case AArch64::FMOVDr: case AArch64::FMOVSr: case AArch64::FMOVHr:
      writeReg(T, Rd, locsOf(MI.getOperand(1).getReg()), true);
      break;
    // A named register read (llvm.read_register, __getReg): mov Xt, Xn or
    // fmov Dt, Dn, the source register held in an immediate operand.
    case AArch64::READ_REGISTER_GPR64:
      writeReg(T, Rd, locsOf(MI.getOperand(1).getImm()));
      break;
    case AArch64::READ_REGISTER_FPR64:
      writeReg(T, Rd, locsOf(MI.getOperand(1).getImm()), true);
      break;
    case AArch64::FMOVDXr: case AArch64::FMOVXDr:
    case AArch64::FMOVSWr: case AArch64::FMOVWSr:
    case AArch64::FMOVHWr: case AArch64::FMOVHXr:
    case AArch64::FMOVWHr: case AArch64::FMOVXHr:
      genDataflow(T, MI);
      break;
    // The upper half of a V register from or to an X register; the lower
    // half is kept: (Rd, Rn, idx).
    case AArch64::FMOVXDHighr:
      T.assign(hbng::vBytes(TRI->getEncodingValue(Rd), 8, 8),
               locsOf(MI.getOperand(1).getReg()));
      break;
    case AArch64::FMOVDXHighr:
      writeReg(T, Rd,
               hbng::vBytes(TRI->getEncodingValue(MI.getOperand(1).getReg()),
                            8, 8));
      break;
    // Scalar copy of a lane (also written MOV): (Rd, Vn, idx), byte for byte
    // ("per 1"), the bytes above the scalar cleared.
    case AArch64::DUPi8: case AArch64::DUPi16:
    case AArch64::DUPi32: case AArch64::DUPi64: {
      unsigned E = regBytes(Rd);
      unsigned Rn = TRI->getEncodingValue(MI.getOperand(1).getReg());
      writeReg(T, Rd, hbng::vBytes(Rn, MI.getOperand(2).getImm() * E, E),
               true);
      break;
    }
    // CRC32 (and CRC32C): the new checksum from the old one and the data.
    case AArch64::CRC32Brr: case AArch64::CRC32Hrr:
    case AArch64::CRC32Wrr: case AArch64::CRC32Xrr:
    case AArch64::CRC32CBrr: case AArch64::CRC32CHrr:
    case AArch64::CRC32CWrr: case AArch64::CRC32CXrr:
      genDataflow(T, MI);
      break;
    // AES and SHA (+aes, +sha2): every byte written from every byte read,
    // the old destination included when it is a source (aese, sha256h).
    case AArch64::AESErr: case AArch64::AESDrr:
    case AArch64::AESMCrr: case AArch64::AESIMCrr:
    case AArch64::SHA1Hrr: case AArch64::SHA1Crrr:
    case AArch64::SHA1Prrr: case AArch64::SHA1Mrrr:
    case AArch64::SHA1SU0rrr: case AArch64::SHA1SU1rr:
    case AArch64::SHA256Hrrr: case AArch64::SHA256H2rrr:
    case AArch64::SHA256SU0rr: case AArch64::SHA256SU1rrr:
      genDataflow(T, MI);
      break;
    //=== BITMANIP ===//
    case AArch64::SBFMXri: case AArch64::SBFMWri:
    case AArch64::UBFMXri: case AArch64::UBFMWri:
    case AArch64::BFMWri: case AArch64::BFMXri:
    case AArch64::EXTRWrri: case AArch64::EXTRXrri:
      genDataflow(T, MI);
      break;

    // Bit count and reverse
    case AArch64::CLZWr: case AArch64::CLZXr:
    case AArch64::CLSWr: case AArch64::CLSXr:
    case AArch64::RBITWr: case AArch64::RBITXr:
    case AArch64::REVWr: case AArch64::REVXr:
    case AArch64::REV16Wr: case AArch64::REV16Xr: case AArch64::REV32Xr:
      genDataflow(T, MI);
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
      // The return address, a constant, into LR.
      T.assign({hbng::Loc::x(30)}, {});
      break;

    //=== CONDITIONAL OPERATIONS ===//
    // Both data operands, not the condition.
    case AArch64::CSELWr: case AArch64::CSELXr: case AArch64::CSINCWr: case AArch64::CSINCXr:
    case AArch64::CSINVWr: case AArch64::CSINVXr: case AArch64::CSNEGWr: case AArch64::CSNEGXr:
    case AArch64::FCSELSrrr: case AArch64::FCSELDrrr:
      genDataflow(T, MI);
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

    // A system register carries no tag (like the flags): its value is not
    // data the program read or received (thread pointer, FPCR/FPSR, counters).
    case AArch64::MRS: case AArch64::MRS_FPCR:
      genClear(T, MI);
      break;
    // Writing one drops the tag (NZCV saved and restored around a call too).
    case AArch64::MSR: case AArch64::MSR_FPCR:
      break;
    // Masking or unmasking exceptions (msr daifset, #2: IRQs masked) moves no
    // data. Not the other PSTATE fields: spsel changes which register sp is.
    case AArch64::MSRpstateImm4: {
      unsigned Field = MI.getOperand(0).getImm();
      if (Field != AArch64PState::DAIFSet && Field != AArch64PState::DAIFClr)
        T.unsupported(TII->getName(MI.getOpcode()));
      break;
    }
    // Barriers, hints and prefetches move no data.
    case AArch64::PRFMui: case AArch64::PRFMl: case AArch64::PRFMroW:
    case AArch64::PRFMroX: case AArch64::PRFUMi:
    case AArch64::DMB: case AArch64::DSB: case AArch64::ISB: case AArch64::HINT:
    case AArch64::NOP:             // Cortex-A53 erratum 835769 workaround
    case AArch64::CLREX:           // Cancel exclusive access
    case AArch64::CFI_INSTRUCTION:
    case AArch64::DBG_VALUE:
    case AArch64::DBG_VALUE_LIST:
    case AArch64::DBG_LABEL:
    case AArch64::DBG_PHI:
    case AArch64::DBG_INSTR_REF:
    case AArch64::IMPLICIT_DEF:
    case AArch64::JUMP_TABLE_DEBUG_INFO:
    case AArch64::JumpTableDest32:
    case AArch64::JumpTableDest16:
    case AArch64::JumpTableDest8:
    case AArch64::KILL:
    case AArch64::HBNG_ATOM_LABEL:
    case AArch64::PACM:            // Modifier for the next PAC (hint space)
    case AArch64::BRK: case AArch64::HLT:  // Traps
      break;
    // Pointer authentication, in the hint space (a NOP without PAC, as on the
    // Cortex-A53): the pointer signed, authenticated or stripped, from the
    // pointer and the modifier (SP, X16 or none).
    case AArch64::PACIASP: case AArch64::PACIBSP:
    case AArch64::AUTIASP: case AArch64::AUTIBSP:
      T.assign({hbng::Loc::x(30)}, {hbng::Loc::x(30), hbng::Loc::sp()});
      break;
    case AArch64::PACIAZ: case AArch64::PACIBZ:
    case AArch64::AUTIAZ: case AArch64::AUTIBZ:
    case AArch64::XPACLRI:
      break; // LR <- LR
      break;
    case AArch64::PACIA1716: case AArch64::PACIB1716:
    case AArch64::AUTIA1716: case AArch64::AUTIB1716:
      T.assign({hbng::Loc::x(17)}, {hbng::Loc::x(17), hbng::Loc::x(16)});
      break;
    default:
      T.unsupported(TII->getName(MI.getOpcode()));
      LLVM_DEBUG(dbgs() << "unsupported " << TII->getName(MI.getOpcode())
                        << "\n");
      break;
  }
  return T;
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");

    // A hbng_no_instr function is uncompiled code for HBNG: no annotations, no
    // block table entries (and no sends, see the STM send pass). It runs, for
    // instance, before the STM address is in place (crt's _start_c).
    const Function &F = MF.getFunction();
    if (F.hasFnAttribute("hbng_no_instr")) {
      LLVM_DEBUG(dbgs() << "Skipping annotations for " << F.getName() << "\n");
      return false;
    }


    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MRI = &MF.getRegInfo();
    // Function Info, the basic block table is stored here
    AArch64FunctionInfo *FI = MF.getInfo<AArch64FunctionInfo>();

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    // Each block's transfers, one per instruction, with the block's index in
    // BBAnnotationInfos. Nothing is printed before the whole function is
    // built, so that later steps can work across its blocks.
    std::vector<std::pair<size_t, std::vector<hbng::Transfer>>> Blocks;
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
      // The current block's transfers, one per instruction.
      std::vector<hbng::Transfer> Groups;
      auto Close = [&] {
        BBAnnotationInfo.NumInstrs = Groups.size();
        Blocks.push_back({FI->BBAnnotationInfos.size(), std::move(Groups)});
        Groups.clear();
        FI->BBAnnotationInfos.push_back(BBAnnotationInfo);
      };

      for (auto MIIt = MBB.begin(), End = MBB.end(); MIIt != End; ++MIIt) {
        MachineInstr &MI = *MIIt;
        unsigned int Opcode = MI.getOpcode();
        StringRef Mnemonic = TII->getName(Opcode);
        LLVM_DEBUG(dbgs() << "instr: " << Mnemonic << "\n");
        for (hbng::Transfer &T : generateGroups(MI)) {
          // One channel entry per send, at this group's index.
          for (size_t I = 0; I < T.Sends.size(); ++I)
            BBAnnotationInfo.SendIndices.push_back(Groups.size());
          Groups.push_back(std::move(T));
        }

        // Check if the current instruction is a branch/return/call etc. but the
        // basic block does not end on it. If this is the case, we have to emit
        // a symbol and push it to the bbt to comply to the CoreSight way of defining
        // Atoms.
        bool IsLastInstr = std::next(MIIt) == End;
        bool IsCtrlFlow = MI.isBranch() || MI.isReturn() || MI.isCall();
        if ((IsCtrlFlow || containsSVC(MI)) && !IsLastInstr) {
          Close();

          // Insert the pseudo instruction in the MBB
          BuildMI(MBB, std::next(MIIt), DebugLoc(), TII->get(AArch64::HBNG_ATOM_LABEL));
          // The AsmPrinter finds the branches to a block by scanning its
          // predecessor's terminators back from the end; the pseudo stops the
          // scan, hiding this branch. Its targets must keep their labels (a
          // branch to the next block, e.g. tbnz + b from branch relaxation).
          for (MachineOperand &MO : MI.operands())
            if (MO.isMBB())
              MO.getMBB()->setLabelMustBeEmitted();

          // Create the new symbols, splitting the basic block and annotations
          std::string SplitBaseName = SymbolName + "_split_" + std::to_string(SplitIndex++);
          MCSymbol *SplitBBSym = MF.getContext().getOrCreateSymbol(SplitBaseName);
          MCSymbol *SplitAnnotSym = MF.getContext().getOrCreateSymbol(SplitBaseName + "_annot");
          // Reset the BBAnnotation
          BBAnnotationInfo.BBSymbol = SplitBBSym;
          BBAnnotationInfo.BBAnnotSymbol = SplitAnnotSym;
          BBAnnotationInfo.Annotations = {};
          BBAnnotationInfo.NumInstrs = 0;
          BBAnnotationInfo.SendIndices.clear();

          LLVM_DEBUG(dbgs() << "Splitting the block, resetting info with: " << SplitBBSym->getName() << " and " << SplitAnnotSym->getName() << "\n");
        }
      }

      // Every block is closed, including one that falls through into the
      // next: the executor continues at the next block when the last
      // instruction is not a branch.
      Close();
    }

    // A block's text: for each instruction, "@<offset>" then its statements;
    // then END. A block with no instruction (only pseudos that emit nothing)
    // has no text, and the block table leaves it out (see
    // emitHBNGBasicBlockTableSection).
    for (const auto &[Index, Groups] : Blocks) {
      std::vector<std::string> &Text = FI->BBAnnotationInfos[Index].Annotations;
      for (size_t I = 0; I < Groups.size(); ++I) {
        Text.push_back("@" + std::to_string(4 * I) + "\n");
        std::string Statements = hbng::printTransfer(Terms, Groups[I]);
        SmallVector<StringRef, 8> Lines;
        StringRef(Statements).split(Lines, '\n', -1, false);
        for (StringRef Line : Lines)
          Text.push_back((Line + "\n").str());
      }
      if (!Groups.empty())
        Text.push_back("END\n");
    }
    Terms.clear();
    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}