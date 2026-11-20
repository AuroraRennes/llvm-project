//===- llvm-hbng-postlink.cpp - Sort, index and check the HBNG table ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runs on a linked AArch64 executable: sorts the block table (.hbngbbt) in
// place by block address, adds its index (.hbngidx) and the format version
// (.hbnginfo), and fails when the table does not describe the code:
//   - an entry outside an executable section, or above 32 bits;
//   - an annotation address outside .hbngannot;
//   - overlapping entries;
//   - a waypoint bit that does not match the block's last instruction;
//   - a block that can continue (no waypoint, or ending in a conditional
//     branch, a call or an isb) not followed by the next entry, directly or
//     after nops (alignment padding, executed with no effect), or by a udf
//     (a call or system call that does not return: reaching it would trap);
//   - executable code without an entry, other than padding (nop, udf) and the
//     ranges declared in .hbngskip.
// The layout is in llvm/Support/HBNGFormat.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ObjCopy/CommonConfig.h"
#include "llvm/ObjCopy/ELF/ELFConfig.h"
#include "llvm/ObjCopy/ELF/ELFObjcopy.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/HBNGFormat.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

using namespace llvm;
using namespace llvm::object;
namespace endian = llvm::support::endian;

static cl::OptionCategory Cat("llvm-hbng-postlink options");
static cl::opt<std::string> InputFile(cl::Positional, cl::Required,
                                      cl::desc("<executable>"), cl::cat(Cat));
static cl::opt<std::string>
    OutputFile("o", cl::desc("Output file (default: replace the input)"),
               cl::value_desc("file"), cl::cat(Cat));
static cl::opt<unsigned>
    MaxErrors("max-errors", cl::init(20),
              cl::desc("Errors printed before the count (0: all)"),
              cl::cat(Cat));

namespace {

struct Entry {
  uint64_t Addr;
  uint64_t Annot;
  uint16_t Count; // without the waypoint bit
  bool Waypoint;
  ArrayRef<uint8_t> Bytes;
  uint64_t end() const { return Addr + 4 * uint64_t(Count); }
};

struct Range {
  uint64_t Start, End;
};

struct CodeSection {
  StringRef Name;
  uint64_t Addr;
  ArrayRef<uint8_t> Data;
  uint64_t end() const { return Addr + Data.size(); }
};

// What a block's last instruction does to the trace and to the control flow.
enum class Ending { None, Continues, Stops };

// Word masks: a waypoint is a branch, call, return or isb. Conditional
// branches, calls and isb can continue to the next instruction.
Ending classify(uint32_t W) {
  if ((W & 0x7c000000) == 0x14000000) // b, bl
    return (W >> 31) ? Ending::Continues : Ending::Stops;
  if ((W & 0xff000000) == 0x54000000) // b.cond, bc.cond
    return Ending::Continues;
  if ((W & 0x7e000000) == 0x34000000) // cbz, cbnz
    return Ending::Continues;
  if ((W & 0x7e000000) == 0x36000000) // tbz, tbnz
    return Ending::Continues;
  if ((W & 0xfe000000) == 0xd6000000) // br, blr, ret and their pauth forms
    return ((W >> 21) & 7) == 1 ? Ending::Continues : Ending::Stops;
  if ((W & 0xfffff0ff) == 0xd50330df) // isb
    return Ending::Continues;
  return Ending::None;
}

std::string hex(uint64_t V) { return "0x" + utohexstr(V, /*LowerCase=*/true); }

constexpr uint32_t Nop = 0xd503201f;

bool isUdf(uint32_t W) { return (W >> 16) == 0; }

bool isPadding(uint32_t W) { return W == Nop || isUdf(W); }

class PostLink {
public:
  PostLink(const ELF64LEObjectFile &Obj) : Obj(Obj), ELF(Obj.getELFFile()) {}
  Error run(SmallVectorImpl<char> &Table, SmallVectorImpl<char> &Index);
  unsigned numErrors() const { return NumErrors; }

private:
  const ELF64LEObjectFile &Obj;
  const ELFFile<ELF64LE> &ELF;
  std::vector<CodeSection> Code;
  std::vector<Entry> Entries;
  std::vector<Range> Skips;
  std::map<uint64_t, StringRef> Symbols;
  unsigned NumErrors = 0;

  void error(const Twine &Msg) {
    ++NumErrors;
    if (MaxErrors == 0 || NumErrors <= MaxErrors)
      WithColor::error(errs(), "llvm-hbng-postlink")
          << InputFile << ": " << Msg << "\n";
  }
  std::string where(uint64_t Addr) const;
  const CodeSection *codeAt(uint64_t Addr) const;
  uint32_t wordAt(uint64_t Addr) const;
  Error readTable(ArrayRef<uint8_t> Data);
  Error readSkips(ArrayRef<uint8_t> Data);
};

std::string PostLink::where(uint64_t Addr) const {
  std::string S;
  raw_string_ostream OS(S);
  OS << format_hex(Addr, 10);
  auto It = Symbols.upper_bound(Addr);
  const CodeSection *C = codeAt(Addr);
  if (C && It != Symbols.begin() && codeAt((--It)->first) == C) {
    OS << " (" << It->second;
    if (Addr != It->first)
      OS << "+" << format_hex(Addr - It->first, 1);
    OS << ")";
  }
  return S;
}

const CodeSection *PostLink::codeAt(uint64_t Addr) const {
  for (const CodeSection &C : Code)
    if (Addr >= C.Addr && Addr < C.end())
      return &C;
  return nullptr;
}

uint32_t PostLink::wordAt(uint64_t Addr) const {
  const CodeSection *C = codeAt(Addr);
  assert(C && Addr + 4 <= C->end());
  return endian::read32le(C->Data.data() + (Addr - C->Addr));
}

Error PostLink::readTable(ArrayRef<uint8_t> Data) {
  size_t Off = 0;
  while (Off < Data.size()) {
    if (Data.size() - Off < hbng::EntryHeaderSize)
      return createStringError("truncated entry at offset " + Twine(Off) +
                               " of " + hbng::TableSection);
    const uint8_t *P = Data.data() + Off;
    uint16_t Count = endian::read16le(P + 8);
    uint16_t NumSends = endian::read16le(P + 10);
    size_t Size = hbng::entrySize(NumSends);
    if (Data.size() - Off < Size)
      return createStringError("truncated entry at offset " + Twine(Off) +
                               " of " + hbng::TableSection);
    Entries.push_back({endian::read32le(P), endian::read32le(P + 4),
                       uint16_t(Count & ~hbng::WaypointBit),
                       (Count & hbng::WaypointBit) != 0,
                       Data.slice(Off, Size)});
    Off += Size;
  }
  return Error::success();
}

Error PostLink::readSkips(ArrayRef<uint8_t> Data) {
  if (Data.size() % hbng::SkipRecordSize)
    return createStringError(Twine(hbng::SkipSection) +
                             " is not a whole number of ranges");
  for (size_t Off = 0; Off < Data.size(); Off += hbng::SkipRecordSize)
    Skips.push_back({endian::read32le(Data.data() + Off),
                     endian::read32le(Data.data() + Off + 4)});
  return Error::success();
}

Error PostLink::run(SmallVectorImpl<char> &Table,
                    SmallVectorImpl<char> &Index) {
  if (ELF.getHeader().e_machine != ELF::EM_AARCH64)
    return createStringError("not an AArch64 file");
  if (ELF.getHeader().e_type != ELF::ET_EXEC)
    return createStringError("not a linked executable");

  auto Sections = ELF.sections();
  if (!Sections)
    return Sections.takeError();
  std::optional<ArrayRef<uint8_t>> TableData, SkipData;
  std::optional<Range> Annot;
  for (const auto &Sec : *Sections) {
    Expected<StringRef> Name = ELF.getSectionName(Sec);
    if (!Name)
      return Name.takeError();
    if (*Name == hbng::IndexSection || *Name == hbng::InfoSection)
      return createStringError("already has " + *Name);
    bool Alloc = Sec.sh_flags & ELF::SHF_ALLOC;
    if (Alloc && (Sec.sh_flags & ELF::SHF_EXECINSTR) &&
        Sec.sh_type == ELF::SHT_PROGBITS && Sec.sh_size) {
      auto Data = ELF.getSectionContents(Sec);
      if (!Data)
        return Data.takeError();
      Code.push_back({*Name, Sec.sh_addr, *Data});
      if (Sec.sh_addr + Sec.sh_size > (1ull << 32))
        error("section " + *Name + " ends above 32 bits");
    }
    if (*Name == hbng::TableSection || *Name == hbng::SkipSection) {
      auto Data = ELF.getSectionContents(Sec);
      if (!Data)
        return Data.takeError();
      (*Name == hbng::TableSection ? TableData : SkipData) = *Data;
    }
    if (*Name == hbng::AnnotSection)
      Annot = Range{Sec.sh_addr, Sec.sh_addr + Sec.sh_size};
  }
  if (!TableData)
    return createStringError(Twine("no ") + hbng::TableSection);
  if (!Annot)
    return createStringError(Twine("no ") + hbng::AnnotSection);
  if (Error E = readTable(*TableData))
    return E;
  if (SkipData)
    if (Error E = readSkips(*SkipData))
      return E;

  for (const ELFSymbolRef &Sym : Obj.symbols()) {
    Expected<StringRef> Name = Sym.getName();
    Expected<uint64_t> Addr = Sym.getAddress();
    Expected<SymbolRef::Type> Type = Sym.getType();
    if (!Name || !Addr || !Type) {
      consumeError(Name.takeError());
      consumeError(Addr.takeError());
      consumeError(Type.takeError());
      continue;
    }
    if (*Type == SymbolRef::ST_Function && !Name->empty())
      Symbols.try_emplace(*Addr, *Name);
  }

  // The entries, sorted by address, each inside one executable section.
  llvm::stable_sort(
      Entries, [](const Entry &A, const Entry &B) { return A.Addr < B.Addr; });
  for (const Entry &E : Entries) {
    const CodeSection *C = codeAt(E.Addr);
    if (E.Count == 0)
      error("empty block at " + where(E.Addr));
    else if (E.Addr % 4)
      error("misaligned block at " + where(E.Addr));
    else if (!C || E.end() > C->end())
      error("block at " + where(E.Addr) +
            " is not inside an executable section");
    if (E.Annot < Annot->Start || E.Annot >= Annot->End)
      error("block at " + where(E.Addr) + ": annotations at " + hex(E.Annot) +
            " outside " + hbng::AnnotSection);
  }
  if (NumErrors)
    return Error::success();

  for (size_t I = 0; I < Entries.size(); ++I) {
    const Entry &E = Entries[I];
    const Entry *Next = I + 1 < Entries.size() ? &Entries[I + 1] : nullptr;
    if (Next && Next->Addr < E.end()) {
      error("blocks at " + where(E.Addr) + " and " + where(Next->Addr) +
            " overlap");
      continue;
    }
    uint64_t Last = E.end() - 4;
    Ending End = classify(wordAt(Last));
    if (E.Waypoint != (End != Ending::None)) {
      error("block at " + where(E.Addr) +
            (E.Waypoint ? " has the waypoint bit but ends in "
                        : " has no waypoint bit but ends in ") +
            hex(wordAt(Last)) + " at " + where(Last));
      continue;
    }
    if (End == Ending::Stops)
      continue;
    const CodeSection *C = codeAt(Last);
    bool Trap = E.end() < C->end() && isUdf(wordAt(E.end()));
    bool Nops = Next && Next->Addr <= C->end();
    for (uint64_t A = E.end(); Nops && A < Next->Addr; A += 4)
      Nops = wordAt(A) == Nop;
    if (!Nops && !Trap)
      error("block at " + where(E.Addr) +
            " can continue but is not followed by an entry at " +
            where(E.end()));
  }

  // Coverage: every word of code is in an entry, in a skipped range, or
  // padding.
  llvm::sort(Skips,
             [](const Range &A, const Range &B) { return A.Start < B.Start; });
  for (const Range &S : Skips)
    if (S.End < S.Start || S.Start % 4 || S.End % 4 || !codeAt(S.Start) ||
        codeAt(S.Start) != codeAt(S.End - 4))
      error("bad skipped range " + where(S.Start) + " to " + hex(S.End));
  for (const Entry &E : Entries)
    for (const Range &S : Skips)
      if (S.Start < E.end() && E.Addr < S.End)
        error("skipped range at " + where(S.Start) + " overlaps block at " +
              where(E.Addr));
  for (const CodeSection &C : Code) {
    auto EIt =
        llvm::lower_bound(Entries, C.Addr, [](const Entry &E, uint64_t A) {
          return E.end() <= A;
        });
    auto SIt = llvm::lower_bound(
        Skips, C.Addr, [](const Range &S, uint64_t A) { return S.End <= A; });
    std::optional<uint64_t> Gap;
    auto Report = [&](uint64_t To) {
      if (Gap)
        error("code without an entry from " + where(*Gap) + " to " + hex(To));
      Gap.reset();
    };
    for (uint64_t A = C.Addr & ~3ull; A + 4 <= C.end(); A += 4) {
      while (EIt != Entries.end() && EIt->end() <= A)
        ++EIt;
      while (SIt != Skips.end() && SIt->End <= A)
        ++SIt;
      bool Covered = (EIt != Entries.end() && EIt->Addr <= A) ||
                     (SIt != Skips.end() && SIt->Start <= A) ||
                     isPadding(wordAt(A));
      if (Covered)
        Report(A);
      else if (!Gap)
        Gap = A;
    }
    Report(C.end());
  }

  // The sorted table and its index.
  for (const Entry &E : Entries) {
    char Rec[hbng::IndexRecordSize];
    endian::write32le(Rec, E.Addr);
    endian::write32le(Rec + 4, Table.size());
    Index.append(Rec, Rec + sizeof(Rec));
    Table.append(E.Bytes.begin(), E.Bytes.end());
  }
  assert(Table.size() == TableData->size());
  return Error::success();
}

[[noreturn]] void fail(Error E) {
  WithColor::error(errs(), "llvm-hbng-postlink")
      << InputFile << ": " << toString(std::move(E)) << "\n";
  exit(1);
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv,
                              "Sort, index and check the HBNG block table\n");

  auto Buf = MemoryBuffer::getFile(InputFile, /*IsText=*/false,
                                   /*RequiresNullTerminator=*/false);
  if (!Buf)
    fail(errorCodeToError(Buf.getError()));
  auto Bin = createBinary((*Buf)->getMemBufferRef());
  if (!Bin)
    fail(Bin.takeError());
  auto *Obj = dyn_cast<ELF64LEObjectFile>(Bin->get());
  if (!Obj)
    fail(createStringError("not a 64-bit little-endian ELF file"));

  SmallVector<char, 0> Table, Index;
  PostLink P(*Obj);
  if (Error E = P.run(Table, Index))
    fail(std::move(E));
  if (P.numErrors()) {
    WithColor::error(errs(), "llvm-hbng-postlink")
        << InputFile << ": " << P.numErrors() << " error(s)\n";
    return 1;
  }

  SmallVector<char, 0> Info(hbng::InfoSize);
  endian::write16le(Info.data(), hbng::FormatVersion);

  objcopy::CommonConfig Config;
  Config.InputFilename = InputFile;
  Config.OutputFilename = OutputFile.empty() ? InputFile : OutputFile;
  auto Buffer = [](SmallVectorImpl<char> &V, StringRef Name) {
    return std::make_unique<SmallVectorMemoryBuffer>(std::move(V), Name, false);
  };
  Config.UpdateSection.emplace_back(hbng::TableSection,
                                    Buffer(Table, hbng::TableSection));
  Config.AddSection.emplace_back(hbng::IndexSection,
                                 Buffer(Index, hbng::IndexSection));
  Config.AddSection.emplace_back(hbng::InfoSection,
                                 Buffer(Info, hbng::InfoSection));
  objcopy::ELFConfig ELFConfig;

  SmallVector<char, 0> Out;
  raw_svector_ostream OS(Out);
  if (Error E =
          objcopy::elf::executeObjcopyOnBinary(Config, ELFConfig, *Obj, OS))
    fail(std::move(E));

  auto File = FileOutputBuffer::create(Config.OutputFilename, Out.size(),
                                       FileOutputBuffer::F_executable);
  if (!File)
    fail(File.takeError());
  llvm::copy(Out, (*File)->getBufferStart());
  if (Error E = (*File)->commit())
    fail(std::move(E));
  return 0;
}
