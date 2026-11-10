#include "AArch64.h"
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

    // The groups of MI, one per instruction it is encoded as (none for a
    // pseudo that emits nothing), each the lines of that instruction.
    std::vector<std::vector<std::string>> generateGroups(MachineInstr &MI);
    std::vector<std::vector<std::string>> genInlineAsmGroups(MachineInstr &MI);
    std::vector<std::string> genSendLines(MachineInstr &MI);
    // The lines of an instruction encoded as one instruction.
    std::vector<std::string> generateAnnotation(MachineInstr &MI);
    std::vector<std::string> genTrackedLines(MachineInstr &MI);
    std::vector<std::string> generateTagAnnotation(MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void printStrOperand(MachineOperand &MO, raw_string_ostream &OS);
    std::optional<std::string> gprLoc(Register R);
    std::string vecRange(Register R, unsigned Lo, unsigned Len);
    std::optional<std::string> srcLoc(Register R);
    std::string valueOf(Register R);
    std::vector<std::string> writeReg(Register R, const std::string &Rhs);
    std::vector<std::string> genDataflow(MachineInstr &MI);
    std::vector<std::string> genClear(MachineInstr &MI);
    std::vector<std::string> genLinkAnnotation();
    std::pair<std::string, int64_t> genAddress(MachineInstr &MI,
                                               const HBNGMemAccess &A);
    std::vector<std::string> genMemAnnotation(MachineInstr &MI, const HBNGMemAccess &A);
    std::vector<std::string> genVecMemAnnotation(MachineInstr &MI,
                                                 const HBNGMemAccess &A);
    static bool isUnsupported(ArrayRef<std::string> Lines);
    std::optional<std::vector<std::string>> genNeonAnnotation(MachineInstr &MI);
    std::string vecBytes(Register R, unsigned Lo, unsigned Len, bool Whole);
    std::string vecSource(Register R, unsigned Lo, unsigned Len);
    unsigned regBytes(Register R);
    std::string genTrackedValueLine(Register Dst, Register Src, int64_t Imm);
    std::string genSPWriteLine(MachineInstr &MI, const HBNGSPWrite &W);
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

// Bytes [Lo, Lo + Len) of V<N>.
static std::string vRange(unsigned N, unsigned Lo, unsigned Len) {
  std::string S = "V" + std::to_string(N) + "[" + std::to_string(Lo);
  if (Len > 1)
    S += ":" + std::to_string(Lo + Len - 1);
  return S + "]";
}

// T + B, B in bytes, possibly negative.
static std::string plus(const std::string &T, int64_t B) {
  if (B == 0)
    return T;
  return T + (B < 0 ? " - " : " + ") +
         std::to_string(B < 0 ? -uint64_t(B) : uint64_t(B));
}

// The union of the sources, each once; "0" when there is none.
static std::string unionOf(ArrayRef<std::string> Srcs) {
  SmallVector<std::string, 4> Seen;
  for (const std::string &S : Srcs)
    if (!is_contained(Seen, S))
      Seen.push_back(S);
  return Seen.empty() ? "0" : join(Seen, " | ");
}

// The tag of a general register, none for the zero register.
std::optional<std::string> AArch64HBNGAnnotate::gprLoc(Register R) {
  if (R == AArch64::XZR || R == AArch64::WZR)
    return std::nullopt;
  if (R == AArch64::SP || R == AArch64::WSP)
    return std::string("SP");
  unsigned N = TRI->getEncodingValue(R);
  if (N == 29)
    return std::string("FP");
  if (N == 30)
    return std::string("LR");
  return "X" + std::to_string(N);
}

// Bytes [Lo, Lo + Len) of the V register R.
std::string AArch64HBNGAnnotate::vecRange(Register R, unsigned Lo,
                                          unsigned Len) {
  return vRange(TRI->getEncodingValue(R), Lo, Len);
}

// A source register: its tag, or every byte of an FP/SIMD register.
std::optional<std::string> AArch64HBNGAnnotate::srcLoc(Register R) {
  if (isFPR(R))
    return vecRange(R, 0, regBytes(R));
  return gprLoc(R);
}

// The value of a register at the instruction's entry.
std::string AArch64HBNGAnnotate::valueOf(Register R) {
  if (R == AArch64::SP || R == AArch64::WSP)
    return "<SP>";
  if (R == AArch64::FP)
    return "<FP>";
  return "<X" + std::to_string(TRI->getEncodingValue(R)) + ">";
}

// The lines writing Rhs to the register R: nothing for the zero register. An
// FP/SIMD register written as B, H, S or D has its bytes above cleared (the
// write zeroes the top of the register), on their own line; a clear of all
// of it is one line.
std::vector<std::string> AArch64HBNGAnnotate::writeReg(Register R,
                                                       const std::string &Rhs) {
  if (!isFPR(R)) {
    std::optional<std::string> L = gprLoc(R);
    if (!L)
      return {};
    return {*L + " <- " + Rhs + "\n"};
  }
  unsigned N = TRI->getEncodingValue(R), Bytes = regBytes(R);
  if (Rhs == "0" || Bytes == 16)
    return {vRange(N, 0, Rhs == "0" ? 16 : Bytes) + " <- " + Rhs + "\n"};
  return {vRange(N, 0, Bytes) + " <- " + Rhs + "\n",
          vRange(N, Bytes, 16 - Bytes) + " <- 0\n"};
}

// The destination (operand 0) from the union of every register read.
std::vector<std::string> AArch64HBNGAnnotate::genDataflow(MachineInstr &MI) {
  SmallVector<std::string, 4> Srcs;
  for (unsigned I = 1, E = MI.getNumExplicitOperands(); I < E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (MO.isReg() && MO.isUse())
      if (std::optional<std::string> L = srcLoc(MO.getReg()))
        Srcs.push_back(*L);
  }
  return writeReg(MI.getOperand(0).getReg(), unionOf(Srcs));
}

// The destination (operand 0) cleared: a constant, an address from PC, a
// system register (thread pointer, FPCR/FPSR, counters: not data the program
// read or received).
std::vector<std::string> AArch64HBNGAnnotate::genClear(MachineInstr &MI) {
  return writeReg(MI.getOperand(0).getReg(), "0");
}

//===----------------------------------------------------------------------===//
// SIMD/FP registers
//
// A V register has one tag per byte. A line names the bytes it writes and
// reads, and an operator saying how their tags combine:
//  - Q<n>/D<n>/S<n>/H<n>/B<n>: the 16/8/4/2/1 bytes of V<n> from byte 0. As a
//    destination, the bytes above are cleared (as a W write clears the top of
//    X): the scalar and 64-bit vector forms;
//  - V<n>[lo:hi]: bytes lo to hi of V<n>, inclusive. As a destination, the
//    other bytes are kept (lane inserts, the "2" forms of narrowing);
//  - W/X registers have one tag: V -> X takes the union of the bytes read,
//    X -> V gives the X tag to every byte written.
// Operators (OR is the combination in every case, except cmp):
//  - mov, log: exact bytes, byte k of the destination from byte k of each
//    source (moves, bitwise operations);
//  - add.e, sub.e, ari.e, shi.e, bit.e, cvt.e: union within each element of
//    e bytes (lanes stay apart). A source of one element (a by-element
//    operand, V<n>[lo:hi] of e bytes) is used for every element;
//  - <op>.wid.a.b, <op>.nar.a.b: union per element, a bytes in, b bytes out
//    (widening, narrowing);
//  - pair.e: output element i from input elements 2i and 2i+1 of the
//    sources taken one after the other (pairwise);
//  - red: the union of every byte read into every byte written (reductions);
//  - cmp.e: clean (no tag on comparisons, as for the condition flags);
//  - perm (b0 b1 ...): byte k of the destination from byte bk of the sources
//    taken one after the other (permutes, duplication of a lane);
//  - tbl <table> idx <index>: the union of all table bytes into every byte
//    written; the index is written but not combined (no pointer tainting);
//  - sel <a> <b> mask <m>: byte k from byte k of a and b, not of the mask (a
//    bitwise select is a condition on the data, like csel).
// Unary lines give the operator after the source. Immediates are written as
// sources (Imm(n)), with no tag.

// Bytes [Lo, Lo + Len) of the V register R, in a source or destination.
std::string AArch64HBNGAnnotate::vecBytes(Register R, unsigned Lo, unsigned Len,
                                          bool Whole) {
  unsigned N = TRI->getEncodingValue(R);
  if (Whole) {
    assert(Lo == 0 && "HBNG: whole register from byte 0");
    char C = Len == 16 ? 'Q' : Len == 8 ? 'D' : Len == 4 ? 'S' : Len == 2 ? 'H'
                                                                       : 'B';
    return C + std::to_string(N);
  }
  return "V" + std::to_string(N) + "[" + std::to_string(Lo) + ":" +
         std::to_string(Lo + Len - 1) + "]";
}

// A source: by its size from byte 0 when it starts there.
std::string AArch64HBNGAnnotate::vecSource(Register R, unsigned Lo,
                                           unsigned Len) {
  return vecBytes(R, Lo, Len, Lo == 0 && isPowerOf2_32(Len) && Len <= 16);
}

// The size of a register in bytes.
unsigned AArch64HBNGAnnotate::regBytes(Register R) {
  return TRI->getRegSizeInBits(*TRI->getMinimalPhysRegClass(R))
             .getFixedValue() /
         8;
}

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
  if (I == 0 || I + 1 >= Name.size() || Name[I] != 'v')
    return std::nullopt;
  NeonName R;
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

static NeonFamily getNeonFamily(StringRef Mn, StringRef &Op) {
  auto Is = [&](std::initializer_list<StringRef> L) {
    return llvm::is_contained(L, Mn);
  };
  if (Is({"ADD", "FADD", "SQADD", "UQADD", "SHADD", "UHADD", "SRHADD",
          "URHADD"}))
    return Op = "add", NF_Lane;
  if (Is({"SUB", "FSUB", "SQSUB", "UQSUB", "SHSUB", "UHSUB"}))
    return Op = "sub", NF_Lane;
  if (Is({"MUL", "MLA", "MLS", "PMUL", "SQDMULH", "SQRDMULH", "FMUL", "FMULX",
          "FMLA", "FMLS", "FDIV", "SABD", "UABD", "FABD", "SABA", "UABA",
          "SMAX", "SMIN", "UMAX", "UMIN", "FMAX", "FMIN", "FMAXNM", "FMINNM",
          "NEG", "ABS", "SQABS", "SQNEG", "FNEG", "FABS", "FSQRT", "FRECPE",
          "FRSQRTE", "URECPE", "URSQRTE", "FRECPS", "FRSQRTS", "FRINTA",
          "FRINTI", "FRINTM", "FRINTN", "FRINTP", "FRINTX", "FRINTZ"}))
    return Op = "ari", NF_Lane;
  if (Is({"SHL", "USHR", "SSHR", "URSHR", "SRSHR", "USRA", "SSRA", "URSRA",
          "SRSRA", "SLI", "SRI", "USHL", "SSHL", "URSHL", "SRSHL", "UQSHL",
          "SQSHL", "SQSHLU", "UQRSHL", "SQRSHL"}))
    return Op = "shi", NF_Lane;
  if (Is({"CNT", "CLZ", "CLS", "RBIT"}))
    return Op = "bit", NF_Lane;
  if (Is({"FCVTZS", "FCVTZU", "FCVTAS", "FCVTAU", "FCVTMS", "FCVTMU", "FCVTNS",
          "FCVTNU", "FCVTPS", "FCVTPU", "SCVTF", "UCVTF"}))
    return Op = "cvt", NF_Lane;
  if (Is({"AND", "ORR", "EOR", "BIC", "ORN", "NOT"}))
    return Op = "log", NF_Log;
  if (Is({"CMEQ", "CMGE", "CMGT", "CMHI", "CMHS", "CMLE", "CMLT", "CMTST",
          "FCMEQ", "FCMGE", "FCMGT", "FCMLE", "FCMLT", "FACGE", "FACGT"}))
    return Op = "cmp", NF_Cmp;
  if (Is({"ADDP", "FADDP", "SMAXP", "SMINP", "UMAXP", "UMINP", "FMAXP",
          "FMINP", "FMAXNMP", "FMINNMP"}))
    return Op = "pair", NF_Pair;
  if (Is({"ADDV", "SMAXV", "SMINV", "UMAXV", "UMINV", "SADDLV", "UADDLV",
          "FMAXV", "FMINV", "FMAXNMV", "FMINNMV"}))
    return Op = "red", NF_Red;
  if (Is({"USHLL", "SSHLL", "SHLL"}))
    return Op = "shi", NF_Wide;
  if (Is({"UMULL", "SMULL", "UABDL", "SABDL", "SQDMULL"}))
    return Op = "ari", NF_Wide;
  if (Is({"UADDL", "SADDL"}))
    return Op = "add", NF_Wide;
  if (Is({"USUBL", "SSUBL"}))
    return Op = "sub", NF_Wide;
  if (Is({"FCVTL"}))
    return Op = "cvt", NF_Wide;
  if (Is({"XTN"}))
    return Op = "mov", NF_Narrow;
  if (Is({"SQXTN", "UQXTN", "SQXTUN"}))
    return Op = "ari", NF_Narrow;
  if (Is({"SHRN", "RSHRN", "SQSHRN", "UQSHRN", "SQRSHRN", "UQRSHRN", "SQSHRUN",
          "SQRSHRUN"}))
    return Op = "shi", NF_Narrow;
  if (Is({"FCVTN", "FCVTXN"}))
    return Op = "cvt", NF_Narrow;
  if (Is({"ZIP1", "ZIP2", "UZP1", "UZP2", "TRN1", "TRN2", "EXT", "REV16",
          "REV32", "REV64", "DUP"}))
    return Op = "perm", NF_Perm;
  if (Is({"TBL"}))
    return Op = "tbl", NF_Tbl;
  if (Is({"BSL", "BIT", "BIF"}))
    return Op = "sel", NF_Sel;
  if (Is({"INS"}))
    return Op = "mov", NF_Ins;
  if (Is({"UMOV", "SMOV"}))
    return Op = "mov", NF_Umov;
  if (Is({"MOVI", "MVNI", "FMOV"}))
    return Op = "mov", NF_Imm;
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

// The annotation of a NEON instruction (Advanced SIMD vector and scalar
// forms), std::nullopt if it is not one, or an Unsupported line for a NEON
// mnemonic whose form is not described.
std::optional<std::vector<std::string>>
AArch64HBNGAnnotate::genNeonAnnotation(MachineInstr &MI) {
  StringRef Name = TII->getName(MI.getOpcode());
  std::optional<NeonName> NN = parseNeonName(Name);
  if (!NN)
    return std::nullopt;
  StringRef Op;
  NeonFamily F = getNeonFamily(NN->Mn, Op);
  if (F == NF_None)
    return std::nullopt;
  std::vector<std::string> Unsupported = {
      "Unsupported: " + Name.str() + "\n"};

  unsigned NumOps = MI.getNumExplicitOperands();
  MachineOperand &Dst = MI.getOperand(0);
  if (!Dst.isReg() || !Dst.isDef())
    return Unsupported;
  Register Rd = Dst.getReg();
  // A destination register that is also read (tied): its old value.
  bool Tied = NumOps > 1 && MI.getOperand(1).isReg() &&
              MI.getOperand(1).isTied() && MI.getOperand(1).isUse();
  bool Indexed = NN->Rest.contains("indexed");
  unsigned E = NN->E;
  auto Line = [](const std::string &D, const std::string &Src) {
    std::string L = D + " <- " + Src;
    LLVM_DEBUG(dbgs() << L << "\n");
    return std::vector<std::string>{L + "\n"};
  };
  auto Name_ = [&](Register R) { return TRI->getRegAsmName(R).str(); };
  // A whole register operand, as written by the instruction.
  auto Whole = [&](Register R) {
    if (AArch64::FPR128RegClass.contains(R) ||
        AArch64::FPR64RegClass.contains(R) ||
        AArch64::FPR32RegClass.contains(R) ||
        AArch64::FPR16RegClass.contains(R) || AArch64::FPR8RegClass.contains(R))
      return vecBytes(R, 0, regBytes(R), /*Whole=*/true);
    return Name_(R);
  };
  // Infix line over the explicit operands from First (registers, and the
  // first immediate); a by-element operand (Rm, idx) is its lane.
  auto Infix = [&](unsigned First, StringRef O) {
    SmallVector<std::string, 4> Srcs;
    bool SeenImm = false;
    unsigned Last = Indexed ? NumOps - 2 : NumOps;
    for (unsigned I = First; I < Last; ++I) {
      MachineOperand &MO = MI.getOperand(I);
      if (MO.isReg())
        Srcs.push_back(Whole(MO.getReg()));
      else if (MO.isImm() && !SeenImm) {
        Srcs.push_back("Imm(" + std::to_string(MO.getImm()) + ")");
        SeenImm = true;
      }
    }
    if (Indexed)
      Srcs.push_back(vecBytes(MI.getOperand(NumOps - 2).getReg(),
                              MI.getOperand(NumOps - 1).getImm() * E, E,
                              /*Whole=*/false));
    if (Srcs.size() == 1)
      return Srcs[0] + " " + O.str();
    return llvm::join(Srcs, " " + O.str() + " ");
  };
  std::string OpE = (Op + "." + Twine(E)).str();

  switch (F) {
  case NF_None:
    break;
  case NF_Lane:
    // Lane-wise, the old destination included when it is read (mla, usra).
    return Line(Whole(Rd), Infix(1, OpE));
  case NF_Cmp:
    return Line(Whole(Rd), Infix(1, OpE));
  case NF_Log:
    return Line(Whole(Rd), Infix(1, Op));
  case NF_Pair:
    if (NN->Rest == "p") // scalar pairwise: a reduction of two elements
      return Line(Whole(Rd), Infix(1, "red"));
    return Line(Whole(Rd), Infix(1, OpE));
  case NF_Red:
    return Line(Whole(Rd), Infix(1, Op));
  case NF_Wide: {
    // The source arrangement is in the name; the "2" forms read the upper
    // half of their (128-bit) sources.
    if (Indexed || Tied || NN->N == 0)
      return Unsupported;
    bool Upper = NN->N * E == 16;
    SmallVector<std::string, 3> Srcs;
    for (unsigned I = 1; I < NumOps; ++I) {
      MachineOperand &MO = MI.getOperand(I);
      if (MO.isReg())
        Srcs.push_back(Upper ? vecBytes(MO.getReg(), 8, 8, false)
                             : Whole(MO.getReg()));
      else if (MO.isImm())
        Srcs.push_back("Imm(" + std::to_string(MO.getImm()) + ")");
    }
    std::string O = (Op + ".wid." + Twine(E) + "." + Twine(2 * E)).str();
    std::string S = Srcs.size() == 1 ? Srcs[0] + " " + O
                                     : llvm::join(Srcs, " " + O + " ");
    return Line(Whole(Rd), S);
  }
  case NF_Narrow: {
    // The destination arrangement is in the name; the "2" forms write the
    // upper half of the destination and keep the lower one.
    if (NN->N == 0)
      return Unsupported;
    bool Upper = NN->N * E == 16;
    if (Upper != Tied)
      return Unsupported;
    std::string O = (Op + ".nar." + Twine(2 * E) + "." + Twine(E)).str();
    std::string D = Upper ? vecBytes(Rd, 8, 8, false) : Whole(Rd);
    return Line(D, Infix(Tied ? 2 : 1, O));
  }
  case NF_Perm: {
    if (NN->Mn == "DUP" && NN->Rest == "gpr") // X -> V, every element
      return Line(Whole(Rd), Name_(MI.getOperand(1).getReg()) + " mov");
    if (NN->N == 0)
      return Unsupported;
    unsigned DB = regBytes(Rd);
    unsigned NumSrc = NN->Mn == "DUP" || NN->Mn.starts_with("REV") ? 1 : 2;
    SmallVector<std::string, 2> Srcs;
    for (unsigned I = 1; I <= NumSrc; ++I)
      Srcs.push_back(Whole(MI.getOperand(I).getReg()));
    std::optional<SmallVector<unsigned, 16>> Map = getPermMap(*NN, MI, DB);
    if (!Map)
      return Unsupported;
    std::string S = llvm::join(Srcs, " perm ");
    if (NumSrc == 1)
      S += " perm";
    S += " (";
    for (unsigned K = 0; K < Map->size(); ++K)
      S += (K ? " " : "") + std::to_string((*Map)[K]);
    S += ")";
    return Line(Whole(Rd), S);
  }
  case NF_Tbl: {
    // (Vd, table list, Vm). TBX, which keeps the old byte for an index out
    // of range, is not described (not in the families).
    Register List = MI.getOperand(1).getReg();
    SmallVector<std::string, 4> Table;
    if (AArch64::FPR128RegClass.contains(List)) {
      Table.push_back(Whole(List));
    } else {
      static const unsigned QSub[] = {AArch64::qsub0, AArch64::qsub1,
                                      AArch64::qsub2, AArch64::qsub3};
      for (unsigned Sub : QSub)
        if (Register R = TRI->getSubReg(List, Sub))
          Table.push_back(Whole(R));
    }
    return Line(Whole(Rd), "tbl " + llvm::join(Table, " ") + " idx " +
                               Whole(MI.getOperand(2).getReg()));
  }
  case NF_Sel: {
    // (Vd, Vd tied, Vn, Vm). BSL: Vd is the mask, Vn and Vm the data.
    // BIT/BIF: Vm is the mask, Vd's old bits and Vn the data.
    if (!Tied)
      return Unsupported;
    Register Old = MI.getOperand(1).getReg(), Rn = MI.getOperand(2).getReg(),
             Rm = MI.getOperand(3).getReg();
    if (NN->Mn == "BSL")
      return Line(Whole(Rd), "sel " + Whole(Rn) + " " + Whole(Rm) + " mask " +
                                 Whole(Old));
    return Line(Whole(Rd), "sel " + Whole(Old) + " " + Whole(Rn) + " mask " +
                               Whole(Rm));
  }
  case NF_Ins: {
    // (Vd, Vd tied, idx, Rn [, idx2]): one lane from Rn, the others kept.
    if (!Tied || NN->N != 0)
      return Unsupported;
    std::string D = vecBytes(Rd, MI.getOperand(2).getImm() * E, E, false);
    Register Rn = MI.getOperand(3).getReg();
    if (NN->Rest == "gpr")
      return Line(D, Name_(Rn) + " mov");
    if (NN->Rest == "lane")
      return Line(D, vecSource(Rn, MI.getOperand(4).getImm() * E, E) + " mov");
    return Unsupported;
  }
  case NF_Umov: {
    // (Rd, Vn, idx): one lane to a W/X register.
    if (NN->N != 0)
      return Unsupported;
    return Line(Name_(Rd), vecSource(MI.getOperand(1).getReg(),
                                     MI.getOperand(2).getImm() * E, E) +
                               " mov");
  }
  case NF_Imm:
    // (Vd, imm8 [, shift]); the ORR/BIC immediate forms are NF_Log.
    if (Tied || NumOps < 2 || !MI.getOperand(1).isImm())
      return Unsupported;
    return Line(Whole(Rd), "Imm(" + std::to_string(MI.getOperand(1).getImm()) +
                               ")");
  }
  return Unsupported;
}

// The value of a tracked register (SP, FP) computed from another one:
//   <Dst> <- <Src> + b        (b in bytes, may be negative)
std::string AArch64HBNGAnnotate::genTrackedValueLine(Register Dst, Register Src,
                                                     int64_t Imm) {
  return valueOf(Dst) + " <- " + plus(valueOf(Src), Imm) + "\n";
}

// The value of SP written by MI from sources that are not all tracked (see
// generateAnnotation for the forms).
std::string AArch64HBNGAnnotate::genSPWriteLine(MachineInstr &MI,
                                                const HBNGSPWrite &W) {
  std::string Unsupported =
      "unsupported \"SP <- " + TII->getName(MI.getOpcode()).str() + "\"\n";
  if (!W.Supported)
    return Unsupported;
  std::string Rn = valueOf(MI.getOperand(W.Rn).getReg());
  int64_t Imm = MI.getOperand(2).isImm() ? MI.getOperand(2).getImm() : 0;
  switch (MI.getOpcode()) {
  case AArch64::ADDXri:
  case AArch64::SUBXri: {
    int64_t Bytes = Imm << AArch64_AM::getShiftValue(MI.getOperand(3).getImm());
    return "<SP> <- " + plus(Rn, W.IsSub ? -Bytes : Bytes) + "\n";
  }
  case AArch64::ANDXri: {
    int64_t Mask = AArch64_AM::decodeLogicalImmediate(Imm, 64);
    return "<SP> <- (" + Rn + " & " + std::to_string(Mask) + ")\n";
  }
  case AArch64::ORRXri:
  case AArch64::EORXri:
    return Unsupported; // no such term
  default: { // ADD/SUB extended register
    unsigned Ext = MI.getOperand(3).getImm();
    const char *Kind;
    switch (AArch64_AM::getArithExtendType(Ext)) {
    case AArch64_AM::UXTW: Kind = "uxtw"; break;
    case AArch64_AM::SXTW: Kind = "sxtw"; break;
    case AArch64_AM::UXTX: Kind = "uxtx"; break;
    case AArch64_AM::SXTX: Kind = "sxtx"; break;
    default:
      return Unsupported; // a byte or halfword extension
    }
    return "<SP> <- " + Rn + (W.IsSub ? " - " : " + ") + "ext(" +
           valueOf(MI.getOperand(W.Rm).getReg()) + ", " + Kind + ", " +
           std::to_string(AArch64_AM::getArithShiftValue(Ext)) + ")\n";
  }
  }
}

// The address of a load or store: a term (the base's value, and a register
// offset or LO12 added to it) and an offset in bytes. The address of a
// post-indexed access is the base before the write-back.
std::pair<std::string, int64_t>
AArch64HBNGAnnotate::genAddress(MachineInstr &MI, const HBNGMemAccess &A) {
  std::string Base = valueOf(MI.getOperand(A.Base).getReg());
  if (A.Offset < 0 || A.Mode == HBNGMemAccess::PostIndex)
    return {Base, 0};
  if (A.isRegOffset()) {
    Register Rm = MI.getOperand(A.Offset).getReg();
    bool Signed = MI.getOperand(A.Offset + 1).getImm();
    bool DoShift = MI.getOperand(A.Offset + 2).getImm();
    const char *Kind = A.Mode == HBNGMemAccess::RegOffsetW
                           ? (Signed ? "sxtw" : "uxtw")
                           : (Signed ? "sxtx" : "uxtx");
    return {Base + " + ext(" + valueOf(Rm) + ", " + Kind + ", " +
                std::to_string(DoShift ? Log2_32(A.Size) : 0) + ")",
            0};
  }
  const MachineOperand &Off = MI.getOperand(A.Offset);
  // A symbol's low 12 bits (":lo12:sym", ":got_lo12:sym"): the linker writes
  // the offset into the instruction, LO12 names it.
  if (!Off.isImm())
    return {Base + " + LO12", 0};
  return {Base, Off.getImm() * (A.isScaledImm() ? int64_t(A.Size) : 1)};
}

// Size bytes of memory at Term + Off.
static std::string memBytes(const std::string &Term, int64_t Off,
                            unsigned Size) {
  std::string S = "M[" + plus(Term, Off);
  if (Size > 1)
    S += " .. +" + std::to_string(Size);
  return S + "]";
}

// A call writes the return address, a constant, into LR.
std::vector<std::string> AArch64HBNGAnnotate::genLinkAnnotation() {
  return {"LR <- 0\n"};
}

// Annotations of a load or store: one line per data register, then the
// write-back of a tracked base, then the store-exclusive status:
//   Rt <- M[<Rn> + off .. +size]                  (load)
//   M[<Rn> + off .. +size] <- Rt                  (store)
//   <Rn> <- <Rn> + b                              (write-back of SP or FP)
//   Ws <- 0                                       (store-exclusive status)
// An FP/SIMD register moves byte for byte ("per 1"), a general register has
// one tag for all the bytes. No pointer tainting: the tags of the address
// registers do not reach the data. The write-back leaves the tag of the base
// unchanged (an immediate is added), so it needs a line only for a tracked
// base, whose value it updates; any other base is sent at its next access.
std::vector<std::string> AArch64HBNGAnnotate::genMemAnnotation(
    MachineInstr &MI, const HBNGMemAccess &A) {
  if (A.Vec != HBNGMemAccess::NotVec)
    return genVecMemAnnotation(MI, A);
  std::vector<std::string> Annotations;
  auto [Term, Off] = genAddress(MI, A);
  for (unsigned I = 0; I < A.NumData; ++I) {
    Register Rt = getHBNGDataReg(MI, A, I, TRI);
    std::string Mem = memBytes(Term, Off + I * A.Size, A.Size);
    std::string Per = isFPR(Rt) ? " per 1" : "";
    if (!A.IsStore) {
      for (std::string &L : writeReg(Rt, Mem + Per))
        Annotations.push_back(L);
      continue;
    }
    std::optional<std::string> Src = srcLoc(Rt);
    Annotations.push_back(Mem + " <- " + (Src ? *Src + Per : "0") + "\n");
  }

  Register Base = MI.getOperand(A.Base).getReg();
  if (isHBNGTrackedReg(Base))
    if (std::optional<int64_t> Bytes = getHBNGWritebackBytes(MI, A))
      Annotations.push_back(genTrackedValueLine(Base, Base, *Bytes));

  if (A.Status >= 0)
    for (std::string &L : writeReg(MI.getOperand(A.Status).getReg(), "0"))
      Annotations.push_back(L);
  return Annotations;
}

// Annotations of a structured (NEON) load or store, in the form (one line per
// data register, then the write-back of a tracked base):
//   Rt <- [<Rn> + Off + k]_Bits                     (load)
//   [<Rn> + Off + k]_Bits <- Rt                     (store)
//   <Rn> <- <Rn> + Imm(b)                           (write-back of SP or FP)
//   Ws <- Imm(0)                                    (store-exclusive status)
// No pointer tainting: the tags of the address registers do not reach the
// data. The write-back leaves the tag of the base unchanged (an immediate is
// added), so it needs a line only for a tracked base, whose value it updates
// (b in bytes); any other base is sent at its next access.
// Off is in bytes: "S*Imm(i)" for an immediate in units of the access size S,
// "Imm(i)" for a byte immediate, "<Rm> ext amount" for a register offset, and
// "Imm(0)" for a post-indexed access (the address is the base before the
// write-back). k is the byte offset of the second register of a pair. Bits is
// the width of one data register's access.
std::vector<std::string> AArch64HBNGAnnotate::genVecMemAnnotation(
    MachineInstr &MI, const HBNGMemAccess &A) {
  std::vector<std::string> Annotations;
  MachineOperand &Base = MI.getOperand(A.Base);

  // The offset as written in the instruction.
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
    Register Rt = getHBNGDataReg(MI, A, I, TRI);
    // The first byte of register I, the element size and the data register.
    unsigned First = I * A.Size, Bits = A.Size * 8;
    std::string Data = TRI->getRegAsmName(Rt).str(), Suffix;
    switch (A.Vec) {
    case HBNGMemAccess::NotVec:
    case HBNGMemAccess::VecMultiple:
      break;
    case HBNGMemAccess::VecInterleaved:
      First = I * A.ElemSize;
      Bits = A.ElemSize * 8;
      Suffix = " step " + std::to_string(A.NumData * A.ElemSize) + " x" +
               std::to_string(A.RegBytes / A.ElemSize);
      break;
    case HBNGMemAccess::VecReplicate:
      Suffix = " rep " + std::to_string(A.RegBytes / A.ElemSize);
      break;
    case HBNGMemAccess::VecLane: {
      unsigned Lo = MI.getOperand(A.Lane).getImm() * A.ElemSize;
      Data = vecBytes(Rt, Lo, A.ElemSize, /*Whole=*/false);
      break;
    }
    }

    std::string Addr;
    raw_string_ostream OSAddr(Addr);
    OSAddr << "[<";
    printStrOperand(Base, OSAddr);
    OSAddr << "> + " << AddrOff;
    if (First)
      OSAddr << " + " << First;
    OSAddr << Suffix << "]_" << Bits;

    std::string Result;
    raw_string_ostream OS(Result);
    if (A.IsStore)
      OS << Addr << " <- " << Data;
    else
      OS << Data << " <- " << Addr;
    LLVM_DEBUG(dbgs() << Result << "\n");
    Annotations.push_back(Result + "\n");
  }

  if (isHBNGTrackedReg(Base.getReg()))
    if (std::optional<int64_t> Bytes = getHBNGWritebackBytes(MI, A))
      Annotations.push_back(
          genTrackedValueLine(Base.getReg(), Base.getReg(), *Bytes));

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

// A send: one line per FIFO entry, in the order the STM emits them. A pair
// "stp a, b, [x18]" stores a at the lower address and b at the higher one,
// and the STM emits the higher address first: "send b", then "send a".
// FP is never sent as an address (it is tracked): a send of x29 follows a
// write of FP whose value the annotations cannot compute, and gives it.
std::vector<std::string> AArch64HBNGAnnotate::genSendLines(MachineInstr &MI) {
  std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI);
  std::vector<std::string> Lines;
  bool SendsFP = false;
  for (unsigned I = A->NumData; I-- > 0;) {
    Register R = getHBNGDataReg(MI, *A, I, TRI);
    Lines.push_back("send X" + std::to_string(TRI->getEncodingValue(R)) + "\n");
    SendsFP |= R == AArch64::FP;
  }
  if (SendsFP)
    Lines.push_back("<FP> <- <X29>\n");
  return Lines;
}

// Inline asm, one group per instruction: svc for a system call; any other
// instruction is not described, so it is reported like an unsupported opcode
// rather than dropped. An empty statement (a compiler barrier) has no group.
// A system register access and a barrier are described as their opcodes are:
// msr and barriers annotate nothing, and mrs gives a clean value when the
// statement is the whole asm and has one output register (otherwise which
// output it writes is not known here).
std::vector<std::vector<std::string>>
AArch64HBNGAnnotate::genInlineAsmGroups(MachineInstr &MI) {
  SmallVector<std::string, 4> Mnemonics = getInlineAsmMnemonics(MI);
  SmallVector<Register, 2> Outs;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isDef() && !MO.isImplicit()) // not a clobber
      Outs.push_back(MO.getReg());
  std::vector<std::vector<std::string>> Groups;
  for (const std::string &Mnemonic : Mnemonics) {
    if (Mnemonic == "svc")
      Groups.push_back({"svc\n"});
    else if (is_contained({"msr", "dmb", "dsb", "isb", "nop"}, Mnemonic))
      Groups.push_back({});
    else if (Mnemonic == "mrs" && Mnemonics.size() == 1 && Outs.size() == 1)
      Groups.push_back(writeReg(Outs[0], "0"));
    else
      Groups.push_back({"unsupported \"INLINEASM " + Mnemonic + "\"\n"});
  }
  return Groups;
}

std::vector<std::vector<std::string>>
AArch64HBNGAnnotate::generateGroups(MachineInstr &MI) {
  if (MI.isMetaInstruction() || MI.getOpcode() == AArch64::HBNG_ATOM_LABEL)
    return {};
  if (MI.isInlineAsm()) {
    std::vector<std::vector<std::string>> Groups = genInlineAsmGroups(MI);
    // The tracked registers it writes are known once the statement is done.
    std::vector<std::string> Values = genTrackedLines(MI);
    if (!Groups.empty() && !Values.empty()) {
      if (isUnsupported(Values))
        Groups.back() = Values;
      else if (!isUnsupported(Groups.back()))
        Groups.back().insert(Groups.back().end(), Values.begin(), Values.end());
    }
    return Groups;
  }
  // The sends are not part of the program: the coprocessor receives them.
  if (isHBNGSend(MI))
    return {genSendLines(MI)};
  switch (MI.getOpcode()) {
  // adr, ldr(b|h|sw) and add (the AsmPrinter's lowering), not described.
  case AArch64::JumpTableDest32:
  case AArch64::JumpTableDest16:
  case AArch64::JumpTableDest8:
    return {{}, {}, {}};
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
  std::vector<std::vector<std::string>> Groups(Size / 4);
  Groups[0] = {"unsupported \"" + TII->getName(MI.getOpcode()).str() + "\"\n"};
  return Groups;
}

// The annotations of MI: its tag lines (generateTagAnnotation), then the
// values of the tracked registers it writes (SP, FP, see isHBNGTrackedReg):
//   <Dst> <- <Src> + b                immediate update from a tracked register
//   <SP> <- <Rn> + b                  any other write of SP, from sent sources
//   <SP> <- <Rn> +|- ext(<Rm>, e, s)  (sent before MI unless tracked)
//   <SP> <- (<Rn> & mask)
// with b in bytes (negative for SUB). Any other write of FP sends its value
// right after MI, and that send's group gives it (genSendLines). The
// write-back of an access based on a tracked register is described with the
// access (genMemAnnotation). An unsupported line stands alone in its group.
std::vector<std::string> AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI) {
  std::vector<std::string> Annotations = generateTagAnnotation(MI);
  std::vector<std::string> Values = genTrackedLines(MI);
  if (isUnsupported(Annotations))
    return Annotations;
  if (isUnsupported(Values))
    return Values;
  Annotations.insert(Annotations.end(), Values.begin(), Values.end());
  return Annotations;
}

bool AArch64HBNGAnnotate::isUnsupported(ArrayRef<std::string> Lines) {
  return any_of(Lines, [](const std::string &L) {
    return StringRef(L).starts_with("unsupported ") ||
           StringRef(L).starts_with("Unsupported: ");
  });
}

// The lines of the tracked registers MI writes (see generateAnnotation).
std::vector<std::string> AArch64HBNGAnnotate::genTrackedLines(MachineInstr &MI) {
  std::vector<std::string> Annotations;
  if (std::optional<HBNGTrackedUpdate> U = getHBNGTrackedImmUpdate(MI))
    Annotations.push_back(genTrackedValueLine(U->Dst, U->Src, U->Imm));
  if (std::optional<HBNGSPWrite> W = getHBNGSPWrite(MI, TRI))
    Annotations.push_back(genSPWriteLine(MI, *W));
  return Annotations;
}

std::vector<std::string> AArch64HBNGAnnotate::generateTagAnnotation(MachineInstr &MI) {
  std::vector<std::string> Annotations = {};

  std::string Unsupported =
      "unsupported \"" + TII->getName(MI.getOpcode()).str() + "\"";

  if (TII->isGPRZero(MI))
    return genClear(MI);

  if (std::optional<HBNGMemAccess> A = getHBNGMemAccess(MI))
    return genMemAnnotation(MI, *A);

  if (std::optional<std::vector<std::string>> V = genNeonAnnotation(MI))
    return *V;

  switch (MI.getOpcode()) {
    //=== ARITHMETIC ===//
    // One Register or Immediate
    case AArch64::FNEGSr: case AArch64::FNEGDr:
    case AArch64::FABSSr: case AArch64::FABSDr:
      Annotations = genDataflow(MI);
      break;
    // Two registers
    case AArch64::ADDXrr: case AArch64::ADDXrx: case AArch64::ADDXrs:
    case AArch64::ADDWrr: case AArch64::ADDWrx: case AArch64::ADDWrs:
    case AArch64::FADDSrr: case AArch64::FADDDrr:
      Annotations = genDataflow(MI);
      break;
    case AArch64::SUBXrr: case AArch64::SUBXrx: case AArch64::SUBXrs:
    case AArch64::SUBWrr: case AArch64::SUBWrx: case AArch64::SUBWrs:
    case AArch64::FSUBDrr: case AArch64::FSUBSrr:
      Annotations = genDataflow(MI);
      break;
    case AArch64::ADDSXrr: case AArch64::ADDSXrx: case AArch64::ADDSXrs:
    case AArch64::ADDSWrr: case AArch64::ADDSWrx: case AArch64::ADDSWrs:
      Annotations = genDataflow(MI);
      break;
    case AArch64::SUBSXrr: case AArch64::SUBSXrx: case AArch64::SUBSXrs:
    case AArch64::SUBSWrr: case AArch64::SUBSWrx: case AArch64::SUBSWrs:
      Annotations = genDataflow(MI);
      break;
    case AArch64::ADCSXr: case AArch64::ADCSWr:
    case AArch64::SBCSXr: case AArch64::SBCSWr:
      Annotations = genDataflow(MI);
      break;
    case AArch64::ADCXr: case AArch64::ADCWr:
    case AArch64::SBCXr: case AArch64::SBCWr:
      Annotations = genDataflow(MI);
      break;
    case AArch64::UMULHrr: case AArch64::SMULHrr:
    case AArch64::FMULSrr: case AArch64::FMULDrr:
    case AArch64::FNMULSrr: case AArch64::FNMULDrr:
      Annotations = genDataflow(MI);
      break;
    // Register and Immediate
    case AArch64::ADDXri: case AArch64::ADDWri:
      Annotations = genDataflow(MI);
      break;
    case AArch64::SUBXri: case AArch64::SUBWri:
      Annotations = genDataflow(MI);
      break;
    case AArch64::ADDSXri: case AArch64::ADDSWri:
    case AArch64::SUBSXri: case AArch64::SUBSWri:
      Annotations = genDataflow(MI);
      break;
    case AArch64::UDIVWr: case AArch64::UDIVXr:
    case AArch64::SDIVWr: case AArch64::SDIVXr:
    case AArch64::FDIVSrr: case AArch64::FDIVDrr:
      Annotations = genDataflow(MI);
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
      Annotations = genDataflow(MI);
      break;

    //=== FP MINIMUM AND MAXIMUM ===//
    // The result is one of the data operands, not a comparison result.
    case AArch64::FMAXNMSrr: case AArch64::FMAXNMDrr:
    case AArch64::FMINNMSrr: case AArch64::FMINNMDrr:
    case AArch64::FMAXSrr: case AArch64::FMAXDrr:
    case AArch64::FMINSrr: case AArch64::FMINDrr:
      Annotations = genDataflow(MI);
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
    case AArch64::SCVTFUWHri: case AArch64::SCVTFUWSri: case AArch64::SCVTFUWDri:
    case AArch64::SCVTFUXHri: case AArch64::SCVTFUXSri: case AArch64::SCVTFUXDri:
    case AArch64::UCVTFUWHri: case AArch64::UCVTFUWSri: case AArch64::UCVTFUWDri:
    case AArch64::UCVTFUXHri: case AArch64::UCVTFUXSri: case AArch64::UCVTFUXDri:
      Annotations = genDataflow(MI);
      break;

    //=== ADDR COMPUTATION ===//
    case AArch64::ADR:
      Annotations = genClear(MI);
      break;
    case AArch64::ADRP:
      Annotations = genClear(MI);
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
      Annotations = genDataflow(MI);
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs: case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs: case AArch64::BICSWrr: case AArch64::BICSWrs:
    case AArch64::ANDSXri: case AArch64::ANDSWri:
      Annotations = genDataflow(MI);
      break;
    //=== SHIFTS ===//
    case AArch64::ASRVWr: case AArch64::ASRVXr:
    case AArch64::LSRVWr: case AArch64::LSRVXr:
    case AArch64::LSLVWr: case AArch64::LSLVXr:
      Annotations = genDataflow(MI);
      break;
    //=== MOVES ===//
    // Partial move
    case AArch64::MOVKWi: case AArch64::MOVKXi:
      Annotations = genDataflow(MI);
      break;
    // Total move
    case AArch64::MOVNWi: case AArch64::MOVNXi:
    case AArch64::MOVZXi: case AArch64::MOVZWi:
    case AArch64::MOVID: case AArch64::FMOVSi: case AArch64::FMOVDi:
    case AArch64::FMOVHi:
      Annotations = genDataflow(MI);
      break;
    // Zero
    case AArch64::FMOVS0: case AArch64::FMOVD0:
      Annotations = genClear(MI);
      break;
    // Register move
    case AArch64::FMOVDr: case AArch64::FMOVDXr: case AArch64::FMOVXDr:
    case AArch64::FMOVSr: case AArch64::FMOVSWr: case AArch64::FMOVWSr:
    case AArch64::FMOVHr: case AArch64::FMOVHWr: case AArch64::FMOVHXr:
    case AArch64::FMOVWHr: case AArch64::FMOVXHr:
      Annotations = genDataflow(MI);
      break;
    // The upper half of a V register from or to an X register; the lower
    // half is kept: (Rd, Rn, idx).
    case AArch64::FMOVXDHighr:
      Annotations.push_back(vecRange(MI.getOperand(0).getReg(), 8, 8) + " <- " +
                            *gprLoc(MI.getOperand(1).getReg()) + "\n");
      break;
    case AArch64::FMOVDXHighr:
      Annotations = writeReg(MI.getOperand(0).getReg(),
                             vecRange(MI.getOperand(1).getReg(), 8, 8));
      break;
    // Scalar copy of a lane (also written MOV): (Rd, Vn, idx), the bytes
    // above the scalar cleared.
    case AArch64::DUPi8: case AArch64::DUPi16:
    case AArch64::DUPi32: case AArch64::DUPi64: {
      unsigned E = regBytes(MI.getOperand(0).getReg());
      Register Rn = MI.getOperand(1).getReg();
      Annotations = writeReg(MI.getOperand(0).getReg(),
                             vecRange(Rn, MI.getOperand(2).getImm() * E, E));
      break;
    }
    //=== BITMANIP ===//
    case AArch64::SBFMXri: case AArch64::SBFMWri:
    case AArch64::UBFMXri: case AArch64::UBFMWri:
    case AArch64::BFMWri: case AArch64::BFMXri:
    case AArch64::EXTRWrri: case AArch64::EXTRXrri:
      Annotations = genDataflow(MI);
      break;

    // Bit count and reverse
    case AArch64::CLZWr: case AArch64::CLZXr:
    case AArch64::CLSWr: case AArch64::CLSXr:
    case AArch64::RBITWr: case AArch64::RBITXr:
    case AArch64::REVWr: case AArch64::REVXr:
    case AArch64::REV16Wr: case AArch64::REV16Xr: case AArch64::REV32Xr:
      Annotations = genDataflow(MI);
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
      Annotations = genDataflow(MI);
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
    case AArch64::MRS:
      Annotations = genClear(MI);
      break;
    // Barriers and hints move no data.
    case AArch64::DMB: case AArch64::DSB: case AArch64::ISB: case AArch64::HINT:
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
      // The byte offset of the next instruction in the current block.
      unsigned Offset = 0;
      // A block's text is its groups, "@<offset>" and the lines of the
      // instruction at that offset, closed by END. A block with no
      // instruction (only pseudos that emit nothing) has no text: the block
      // table leaves it out (see emitHBNGBasicBlockTableSection).
      auto Close = [&] {
        if (Offset)
          BBAnnotationInfo.Annotations.push_back("END\n");
        FI->BBAnnotationInfos.push_back(BBAnnotationInfo);
      };

      for (auto MIIt = MBB.begin(), End = MBB.end(); MIIt != End; ++MIIt) {
        MachineInstr &MI = *MIIt;
        unsigned int Opcode = MI.getOpcode();
        StringRef Mnemonic = TII->getName(Opcode);
        LLVM_DEBUG(dbgs() << "instr: " << Mnemonic << "\n");
        for (std::vector<std::string> &Group : generateGroups(MI)) {
          BBAnnotationInfo.Annotations.push_back("@" + std::to_string(Offset) +
                                                 "\n");
          BBAnnotationInfo.Annotations.insert(
              BBAnnotationInfo.Annotations.end(), Group.begin(), Group.end());
          Offset += 4;
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
          Offset = 0;

          LLVM_DEBUG(dbgs() << "Splitting the block, resetting info with: " << SplitBBSym->getName() << " and " << SplitAnnotSym->getName() << "\n");
        }
      }

      // Every block is closed, including one that falls through into the
      // next: the executor continues at the next block when the last
      // instruction is not a branch.
      Close();
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}