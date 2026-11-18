//===-- AArch64HBNGAnnotation.h - HBNG annotation structure -----*- C++ -*-===//
//
// The terms of the HBNG annotations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64HBNGANNOTATION_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64HBNGANNOTATION_H

#include <cstdint>
#include <memory>
#include <string>

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

} // namespace hbng
} // namespace llvm

#endif
