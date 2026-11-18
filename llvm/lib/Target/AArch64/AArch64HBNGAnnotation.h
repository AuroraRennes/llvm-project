//===-- AArch64HBNGAnnotation.h - HBNG annotation structure -----*- C++ -*-===//
//
// The HBNG annotation of an instruction: terms, locations and transfers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64HBNGANNOTATION_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64HBNGANNOTATION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace llvm {
namespace hbng {

//===----------------------------------------------------------------------===//
// Terms

// A value computed from the values at the instruction's entry, modulo 2^64:
// a sum of coefficient * atom, interned in a TermPool (Sum, 0 for none), plus
// Const. The sums are canonical, so two terms are equal iff their fields are.
struct Term {
  unsigned Sum = 0;
  uint64_t Const = 0;
  bool isConst() const { return Sum == 0; }
  bool operator==(const Term &O) const {
    return Sum == O.Sum && Const == O.Const;
  }
  bool operator!=(const Term &O) const { return !(*this == O); }
};

// How a register offset is extended (ldr x0, [x1, w2, sxtw #3]).
enum class Ext { UXTW, SXTW, UXTX, SXTX };

// The terms of one function.
class TermPool {
public:
  TermPool();
  ~TermPool();
  TermPool(const TermPool &) = delete;
  TermPool &operator=(const TermPool &) = delete;

  static Term constant(uint64_t V) { return {0, V}; }
  // Xn's value at the instruction's entry, N below 31.
  Term reg(unsigned N);
  // SP's and FP's values at the instruction's entry, which the annotations
  // track instead of sending.
  Term sp();
  Term fp();
  // The offset the linker writes into the instruction (":lo12:var"). Each
  // instruction has its own, so LO12 in two instructions can be two
  // different numbers.
  Term lo12();
  // The value of the block's I-th channel entry ("S<I>").
  Term send(unsigned I);
  // The LO12 of the instruction at Offset in its block ("LO12@<Offset>").
  Term lo12At(unsigned Offset);

  Term add(Term A, Term B);
  Term sub(Term A, Term B) { return add(A, neg(B)); }
  Term neg(Term A) { return mul(A, ~uint64_t(0)); }
  Term mul(Term A, uint64_t F);
  // A + B for a constant B.
  Term plus(Term A, int64_t B) { return {A.Sum, A.Const + uint64_t(B)}; }
  // ext(A, Kind, Shift), Shift below 64.
  Term ext(Term A, Ext Kind, unsigned Shift);
  // (A & Mask).
  Term mask(Term A, uint64_t Mask);

  // T as the annotations spell it: "<X1> + 8*<X2> - 16".
  std::string str(Term T) const;
  // Orders terms the same way whatever the order they were interned in.
  bool less(Term A, Term B) const;

  // Forgets every term.
  void clear();

private:
  struct Impl;
  std::unique_ptr<Impl> P;
  // The symbol of sort key Key, spelled Text.
  Term symbol(std::string Key, std::string Text);
};

//===----------------------------------------------------------------------===//
// Locations

// One tag cell: X0-X30 and SP (31) one tag each, one byte of a V register, or
// one memory byte.
struct Loc {
  // In the order the statements are printed.
  enum Kind : uint8_t { Mem, Reg, VByte };
  Kind K = Reg;
  uint8_t N = 0; // X register (31 is SP) or V register
  uint8_t B = 0; // V byte
  Term Addr;     // memory byte

  static Loc x(unsigned N) { return {Reg, uint8_t(N), 0, {}}; }
  static Loc sp() { return x(31); }
  static Loc v(unsigned N, unsigned B) {
    return {VByte, uint8_t(N), uint8_t(B), {}};
  }
  static Loc mem(Term A) { return {Mem, 0, 0, A}; }

  auto key() const {
    return std::make_tuple(K, N, B, Addr.Sum, int64_t(Addr.Const));
  }
  bool operator<(const Loc &O) const { return key() < O.key(); }
  bool operator==(const Loc &O) const { return key() == O.key(); }
  bool operator!=(const Loc &O) const { return !(*this == O); }
};

using LocSet = std::set<Loc>;
using LocList = SmallVector<Loc, 16>;

// Bytes [Lo, Lo + Len) of V<N>.
LocList vBytes(unsigned N, unsigned Lo, unsigned Len);
// Size bytes at A, or Times elements of Size bytes, Step bytes apart (step 0:
// the same bytes again).
LocList memBytes(TermPool &Pool, Term A, unsigned Size, unsigned Step = 0,
                 unsigned Times = 1);

//===----------------------------------------------------------------------===//
// Transfers

// What a written location gets: the union of the tags of Srcs. Excl is read
// but not combined (the index of a table lookup, the mask of a select).
struct Dep {
  LocSet Srcs, Excl;
  bool operator==(const Dep &O) const {
    return Srcs == O.Srcs && Excl == O.Excl;
  }
  bool operator!=(const Dep &O) const { return !(*this == O); }
};

// One instruction's annotation: the dependency of each location it writes (a
// location it does not write keeps its tag), the tracked values at the exit,
// the registers it sends, or a statement that stands alone.
struct Transfer {
  std::map<Loc, Dep> Deps;
  // The tracked values at the exit, when they change.
  std::optional<Term> SP, FP;
  // The X registers sent, in the order the channel receives them.
  SmallVector<uint8_t, 2> Sends;
  // svc, or unsupported "Reason": nothing else is said about the instruction.
  enum OpaqueKind : uint8_t { None, Svc, Unsupported };
  OpaqueKind Opaque = None;
  std::string Reason; // of an unsupported statement

  bool isOpaque() const { return Opaque != None; }
  bool empty() const {
    return Deps.empty() && !SP && !FP && Sends.empty() && !isOpaque();
  }

  // Every location of Dsts from all of Srcs (none: a clear), Excl read too.
  void assign(ArrayRef<Loc> Dsts, ArrayRef<Loc> Srcs,
              ArrayRef<Loc> Excl = {});
  // Element i of Dsts (Out locations) from element i (In locations) of each
  // group of Srcs, and of each group of Excl.
  void assignPer(ArrayRef<Loc> Dsts, ArrayRef<ArrayRef<Loc>> Srcs, unsigned In,
                 unsigned Out, ArrayRef<ArrayRef<Loc>> Excl = {});
  // Location i of Dsts from Map[i] alone, cleared when it has none.
  void assignMap(ArrayRef<Loc> Dsts, ArrayRef<std::optional<Loc>> Map);
  // Replaces everything with unsupported "Why".
  void unsupported(StringRef Why) {
    *this = Transfer();
    Opaque = Unsupported;
    Reason = Why.str();
  }
};

// The statements of a transfer, one per line ("...\n" each).
std::string printTransfer(const TermPool &Pool, const Transfer &T);

} // namespace hbng
} // namespace llvm

#endif
