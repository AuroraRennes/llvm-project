//===-- AArch64HBNGBlockForms.h - HBNG block forms --------------*- C++ -*-===//
//
// The groups of a block composed, left to right, into forms.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64HBNGBLOCKFORMS_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64HBNGBLOCKFORMS_H

#include "AArch64HBNGAnnotation.h"
#include "llvm/ADT/DenseMap.h"
#include <optional>
#include <vector>

namespace llvm {
namespace hbng {

// A run of instructions annotated as one: the offset of the first in its
// block, and the transfer of the run, its sends in channel order.
struct Form {
  unsigned Offset;
  Transfer T;
};

// Composes the blocks of one function, whose terms are in Pool.
class BlockForms {
public:
  explicit BlockForms(TermPool &Pool);

  // The forms of a block whose group k is the instruction at offset 4k, or
  // nothing when a register a term reads has no send to name it.
  std::optional<std::vector<Form>> compose(ArrayRef<Group> Groups);

private:
  TermPool &Pool;
  Term SP, FP, LO12;
  // The symbol <Xn>'s sum, to n.
  DenseMap<unsigned, unsigned> RegOf;

  bool then(Transfer &F, const Transfer &Next);
};

} // namespace hbng
} // namespace llvm

#endif
