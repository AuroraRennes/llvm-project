//===- llvm-hbng-postlink.cpp - Sort and index the HBNG table -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runs on a linked AArch64 executable: sorts the block table (.hbngbbt) in
// place by block address, and adds its index (.hbngidx) and the format version
// (.hbnginfo). The layout is in llvm/Support/HBNGFormat.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ObjCopy/CommonConfig.h"
#include "llvm/ObjCopy/ELF/ELFConfig.h"
#include "llvm/ObjCopy/ELF/ELFObjcopy.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/HBNGFormat.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/WithColor.h"

using namespace llvm;
using namespace llvm::object;
namespace endian = llvm::support::endian;

static cl::OptionCategory Cat("llvm-hbng-postlink options");
static cl::opt<std::string> InputFile(cl::Positional, cl::Required,
                                      cl::desc("<executable>"), cl::cat(Cat));
static cl::opt<std::string>
    OutputFile("o", cl::desc("Output file (default: replace the input)"),
               cl::value_desc("file"), cl::cat(Cat));

namespace {

struct Entry {
  uint32_t Addr;
  ArrayRef<uint8_t> Bytes;
};

class PostLink {
public:
  PostLink(const ELFFile<ELF64LE> &ELF) : ELF(ELF) {}
  Error run(SmallVectorImpl<char> &Table, SmallVectorImpl<char> &Index);

private:
  const ELFFile<ELF64LE> &ELF;
  std::vector<Entry> Entries;

  Error readTable(ArrayRef<uint8_t> Data);
};

Error PostLink::readTable(ArrayRef<uint8_t> Data) {
  size_t Off = 0;
  while (Off < Data.size()) {
    if (Data.size() - Off < hbng::EntryHeaderSize)
      return createStringError("truncated entry at offset " + Twine(Off) +
                               " of " + hbng::TableSection);
    const uint8_t *P = Data.data() + Off;
    uint16_t NumSends = endian::read16le(P + 10);
    size_t Size = hbng::entrySize(NumSends);
    if (Data.size() - Off < Size)
      return createStringError("truncated entry at offset " + Twine(Off) +
                               " of " + hbng::TableSection);
    Entries.push_back({endian::read32le(P), Data.slice(Off, Size)});
    Off += Size;
  }
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
  std::optional<ArrayRef<uint8_t>> TableData;
  for (const auto &Sec : *Sections) {
    Expected<StringRef> Name = ELF.getSectionName(Sec);
    if (!Name)
      return Name.takeError();
    if (*Name == hbng::IndexSection || *Name == hbng::InfoSection)
      return createStringError("already has " + *Name);
    if (*Name == hbng::TableSection) {
      auto Data = ELF.getSectionContents(Sec);
      if (!Data)
        return Data.takeError();
      TableData = *Data;
    }
  }
  if (!TableData)
    return createStringError(Twine("no ") + hbng::TableSection);
  if (Error E = readTable(*TableData))
    return E;

  // The table sorted by address, and its index.
  llvm::stable_sort(
      Entries, [](const Entry &A, const Entry &B) { return A.Addr < B.Addr; });
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
                              "Sort and index the HBNG block table\n");

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
  PostLink P(Obj->getELFFile());
  if (Error E = P.run(Table, Index))
    fail(std::move(E));

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
