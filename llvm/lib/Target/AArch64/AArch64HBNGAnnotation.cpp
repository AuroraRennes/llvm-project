//===-- AArch64HBNGAnnotation.cpp - HBNG annotation structure -------------===//
//
// The HBNG annotation structure.
//
//===----------------------------------------------------------------------===//

#include "AArch64HBNGAnnotation.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include <cassert>
#include <deque>

using namespace llvm;
using namespace llvm::hbng;

using u64 = uint64_t;

//===----------------------------------------------------------------------===//
// Terms
//
// A sum is a list of (atom, coefficient), sorted by the atoms' keys, which
// spell the atoms independently of the order they were interned in: the
// symbols (<SP>, <FP>, <X0>-<X30>, LO12), then the extensions, then the
// masks.

namespace {
struct Atom {
  std::string Key;  // sorts the atoms
  std::string Text; // a symbol's name
  enum Kind : uint8_t { Symbol, Extend, Masked } K = Symbol;
  Term Inner;
  Ext ExtKind = Ext::UXTW; // UXTW or SXTW
  unsigned Shift = 0;
  u64 Mask = 0;
};

struct Sum {
  std::vector<std::pair<unsigned, u64>> Coeffs; // atom id, coefficient
  std::string Key;
};
} // namespace

struct TermPool::Impl {
  std::deque<Atom> Atoms;
  StringMap<unsigned> AtomIndex;
  std::deque<Sum> Sums;
  StringMap<unsigned> SumIndex;
  Impl() { Sums.emplace_back(); }

  unsigned atom(Atom A) {
    auto [It, New] = AtomIndex.try_emplace(A.Key, Atoms.size());
    if (New)
      Atoms.push_back(std::move(A));
    return It->second;
  }

  // The term of the sorted coefficients Cs plus C, zero coefficients dropped.
  Term make(std::vector<std::pair<unsigned, u64>> Cs, u64 C) {
    Cs.erase(remove_if(Cs, [](const auto &P) { return P.second == 0; }),
             Cs.end());
    if (Cs.empty())
      return {0, C};
    std::string Key;
    for (const auto &[A, F] : Cs)
      Key += Atoms[A].Key + "*" + utohexstr(F) + ";";
    auto [It, New] = SumIndex.try_emplace(Key, Sums.size());
    if (New)
      Sums.push_back({std::move(Cs), std::move(Key)});
    return {It->second, C};
  }

  Term ofAtom(Atom A) { return make({{atom(std::move(A)), 1}}, 0); }

  bool atomLess(unsigned A, unsigned B) const {
    return Atoms[A].Key < Atoms[B].Key;
  }
};

TermPool::TermPool() : P(std::make_unique<Impl>()) {}
TermPool::~TermPool() = default;

void TermPool::clear() { P = std::make_unique<Impl>(); }

static std::string padded(u64 V, unsigned Width) {
  std::string S = utostr(V);
  return std::string(Width > S.size() ? Width - S.size() : 0, '0') + S;
}

Term TermPool::symbol(std::string Key, std::string Text) {
  Atom A;
  A.Key = "s" + Key;
  A.Text = std::move(Text);
  return P->ofAtom(std::move(A));
}

Term TermPool::sp() { return symbol("0", "<SP>"); }
Term TermPool::fp() { return symbol("1", "<FP>"); }
Term TermPool::reg(unsigned N) {
  return symbol("2" + padded(N, 2), "<X" + utostr(N) + ">");
}
Term TermPool::lo12() { return symbol("3", "LO12"); }

Term TermPool::add(Term A, Term B) {
  if (B.isConst())
    return {A.Sum, A.Const + B.Const};
  if (A.isConst())
    return {B.Sum, A.Const + B.Const};
  const auto &X = P->Sums[A.Sum].Coeffs, &Y = P->Sums[B.Sum].Coeffs;
  std::vector<std::pair<unsigned, u64>> R;
  size_t I = 0, J = 0;
  while (I < X.size() || J < Y.size()) {
    if (J == Y.size() || (I < X.size() && P->atomLess(X[I].first, Y[J].first)))
      R.push_back(X[I++]);
    else if (I == X.size() || P->atomLess(Y[J].first, X[I].first))
      R.push_back(Y[J++]);
    else {
      R.push_back({X[I].first, X[I].second + Y[J].second});
      ++I;
      ++J;
    }
  }
  return P->make(std::move(R), A.Const + B.Const);
}

Term TermPool::mul(Term A, u64 F) {
  if (F == 1)
    return A;
  std::vector<std::pair<unsigned, u64>> R = P->Sums[A.Sum].Coeffs;
  for (auto &C : R)
    C.second *= F;
  return P->make(std::move(R), A.Const * F);
}

Term TermPool::ext(Term A, Ext Kind, unsigned Shift) {
  if (Kind == Ext::UXTX || Kind == Ext::SXTX)
    return mul(A, u64(1) << Shift);
  bool Signed = Kind == Ext::SXTW;
  if (A.isConst()) {
    u64 Low = A.Const & 0xFFFFFFFFu;
    if (Signed && (Low & 0x80000000u))
      Low |= ~u64(0xFFFFFFFFu);
    return constant(Low << Shift);
  }
  Atom At;
  At.K = Atom::Extend;
  At.Inner = A;
  At.ExtKind = Kind;
  At.Shift = Shift;
  At.Key = "x(" + P->Sums[A.Sum].Key + "+" + utohexstr(A.Const) + "," +
           (Signed ? "s" : "u") + "," + utostr(Shift) + ")";
  return P->ofAtom(std::move(At));
}

Term TermPool::mask(Term A, u64 Mask) {
  if (A.isConst())
    return constant(A.Const & Mask);
  if (Mask == ~u64(0))
    return A;
  if (Mask == 0)
    return constant(0);
  Atom At;
  At.K = Atom::Masked;
  At.Inner = A;
  At.Mask = Mask;
  At.Key = "y(" + P->Sums[A.Sum].Key + "+" + utohexstr(A.Const) + "&" +
           utohexstr(Mask) + ")";
  return P->ofAtom(std::move(At));
}

static std::string signedText(u64 V) { return itostr(int64_t(V)); }

std::string TermPool::str(Term T) const {
  std::string R;
  for (const auto &[Id, F] : P->Sums[T.Sum].Coeffs) {
    const Atom &A = P->Atoms[Id];
    std::string Text;
    switch (A.K) {
    case Atom::Symbol:
      Text = A.Text;
      break;
    case Atom::Extend:
      Text = "ext(" + str(A.Inner) + ", " +
             (A.ExtKind == Ext::SXTW ? "sxtw" : "uxtw") +
             ", " + utostr(A.Shift) + ")";
      break;
    case Atom::Masked:
      Text = "(" + str(A.Inner) + " & " + signedText(A.Mask) + ")";
      break;
    }
    bool Neg = int64_t(F) < 0;
    u64 Mag = Neg ? 0 - F : F;
    if (Mag != 1)
      Text = utostr(Mag) + "*" + Text;
    if (R.empty())
      R = Neg ? "-" + Text : Text;
    else
      R += (Neg ? " - " : " + ") + Text;
  }
  if (R.empty())
    return signedText(T.Const);
  if (T.Const)
    R += int64_t(T.Const) < 0 ? " - " + utostr(0 - T.Const)
                              : " + " + utostr(T.Const);
  return R;
}

bool TermPool::less(Term A, Term B) const {
  if (A.Sum != B.Sum)
    return P->Sums[A.Sum].Key < P->Sums[B.Sum].Key;
  return int64_t(A.Const) < int64_t(B.Const);
}

//===----------------------------------------------------------------------===//
// Locations and transfers

LocList llvm::hbng::vBytes(unsigned N, unsigned Lo, unsigned Len) {
  LocList R;
  for (unsigned B = Lo; B < Lo + Len; ++B)
    R.push_back(Loc::v(N, B));
  return R;
}

LocList llvm::hbng::memBytes(TermPool &Pool, Term A, unsigned Size,
                             unsigned Step, unsigned Times) {
  LocList R;
  for (unsigned I = 0; I < Times; ++I)
    for (unsigned K = 0; K < Size; ++K)
      R.push_back(Loc::mem(Pool.plus(A, int64_t(I) * Step + K)));
  return R;
}

static bool isIdentity(const Loc &L, const Dep &D) {
  return D.Srcs.size() == 1 && *D.Srcs.begin() == L && D.Excl.empty();
}

// Writes Deps[i] to Dsts[i]; nothing when each location would keep its own
// tag.
static void write(Transfer &T, ArrayRef<Loc> Dsts, std::vector<Dep> Deps) {
  bool Changes = false;
  for (size_t I = 0; I < Dsts.size(); ++I)
    Changes |= !isIdentity(Dsts[I], Deps[I]);
  if (!Changes)
    return;
  for (size_t I = 0; I < Dsts.size(); ++I) {
    [[maybe_unused]] bool New =
        T.Deps.emplace(Dsts[I], std::move(Deps[I])).second;
    assert(New && "HBNG: a location written twice");
  }
}

void Transfer::assign(ArrayRef<Loc> Dsts, ArrayRef<Loc> Srcs,
                      ArrayRef<Loc> Excl) {
  Dep D;
  D.Srcs.insert(Srcs.begin(), Srcs.end());
  D.Excl.insert(Excl.begin(), Excl.end());
  write(*this, Dsts, std::vector<Dep>(Dsts.size(), D));
}

// Element i of Dsts (Out locations each) from element i (In locations each)
// of each group.
static std::vector<LocSet> perElement(size_t Dsts,
                                      ArrayRef<ArrayRef<Loc>> Groups,
                                      unsigned In, unsigned Out) {
  std::vector<LocSet> R(Dsts);
  for (size_t I = 0; I < Dsts; ++I)
    for (ArrayRef<Loc> G : Groups) {
      ArrayRef<Loc> E = G.slice(I / Out * In, In);
      R[I].insert(E.begin(), E.end());
    }
  return R;
}

void Transfer::assignPer(ArrayRef<Loc> Dsts, ArrayRef<ArrayRef<Loc>> Srcs,
                         unsigned In, unsigned Out,
                         ArrayRef<ArrayRef<Loc>> Excl) {
  std::vector<LocSet> S = perElement(Dsts.size(), Srcs, In, Out);
  std::vector<LocSet> E = perElement(Dsts.size(), Excl, In, Out);
  std::vector<Dep> Deps(Dsts.size());
  for (size_t I = 0; I < Dsts.size(); ++I) {
    Deps[I].Srcs = std::move(S[I]);
    Deps[I].Excl = std::move(E[I]);
  }
  write(*this, Dsts, std::move(Deps));
}

void Transfer::assignMap(ArrayRef<Loc> Dsts,
                         ArrayRef<std::optional<Loc>> Map) {
  std::vector<Dep> Deps(Dsts.size());
  for (size_t I = 0; I < Dsts.size(); ++I)
    if (Map[I])
      Deps[I].Srcs.insert(*Map[I]);
  write(*this, Dsts, std::move(Deps));
}
