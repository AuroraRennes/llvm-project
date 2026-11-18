//===-- AArch64HBNGAnnotation.cpp - HBNG annotation structure -------------===//
//
// The HBNG annotation structure and its text.
//
//===----------------------------------------------------------------------===//

#include "AArch64HBNGAnnotation.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <algorithm>
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

//===----------------------------------------------------------------------===//
// Printing
//
// The written locations, sorted, are cut into runs of consecutive ones, each
// printed in as few statements as the shapes below allow.

namespace {
using Run = std::vector<Loc>;

class Printer {
  const TermPool &Pool;
  std::string &Out;

public:
  Printer(const TermPool &Pool, std::string &Out) : Pool(Pool), Out(Out) {}

  bool less(const Loc &A, const Loc &B) const {
    if (A.K != B.K)
      return A.K < B.K;
    if (A.K == Loc::Mem)
      return Pool.less(A.Addr, B.Addr);
    return std::make_pair(A.N, A.B) < std::make_pair(B.N, B.B);
  }

  std::vector<Loc> sorted(const LocSet &S) const {
    std::vector<Loc> R(S.begin(), S.end());
    std::sort(R.begin(), R.end(),
              [&](const Loc &A, const Loc &B) { return less(A, B); });
    return R;
  }

  static bool follows(const Loc &Prev, const Loc &L, int Dir = 1) {
    if (Prev.K == Loc::VByte && L.K == Loc::VByte)
      return Prev.N == L.N && int(L.B) == int(Prev.B) + Dir;
    if (Prev.K == Loc::Mem && L.K == Loc::Mem && Dir == 1)
      return Prev.Addr.Sum == L.Addr.Sum &&
             L.Addr.Const - Prev.Addr.Const == 1;
    return false;
  }

  // Runs of consecutive locations in the given order: ascending, or
  // descending for V bytes.
  static std::vector<Run> consecutiveRuns(ArrayRef<Loc> Locs) {
    std::vector<Run> Runs;
    for (const Loc &L : Locs) {
      if (!Runs.empty()) {
        Run &R = Runs.back();
        int Dir = R.size() > 1 && follows(R[0], R[1], -1) ? -1 : 1;
        if (follows(R.back(), L, Dir) ||
            (R.size() == 1 && follows(R.back(), L, -1))) {
          R.push_back(L);
          continue;
        }
      }
      Runs.push_back({L});
    }
    return Runs;
  }

  std::string locText(const Loc &L) const {
    switch (L.K) {
    case Loc::Reg:
      return L.N == 31 ? "SP" : L.N == 30 ? "LR" : L.N == 29 ? "FP"
                                                            : "X" + utostr(L.N);
    case Loc::VByte:
      return "V" + utostr(L.N) + "[" + utostr(L.B) + "]";
    case Loc::Mem:
      return "M[" + Pool.str(L.Addr) + "]";
    }
    llvm_unreachable("HBNG: unknown location");
  }

  std::string groupText(const Run &R) const {
    const Loc &First = R.front(), &Last = R.back();
    if (R.size() == 1)
      return locText(First);
    if (First.K == Loc::VByte)
      return "V" + utostr(First.N) + "[" + utostr(First.B) + ":" +
             utostr(Last.B) + "]";
    return "M[" + Pool.str(First.Addr) + " .. +" + utostr(R.size()) + "]";
  }

  // "0", or the runs of S joined with " | ".
  std::string unionText(const LocSet &S) const {
    if (S.empty())
      return "0";
    std::string R;
    for (const Run &Rn : consecutiveRuns(sorted(S)))
      R += (R.empty() ? "" : " | ") + groupText(Rn);
    return R;
  }

  // The same dependency for every location: "V0[0:3] <- X1".
  void unionLine(ArrayRef<Loc> Dsts, const Dep &D) {
    Out += groupText(Dsts) + " <- " + unionText(D.Srcs);
    if (!D.Excl.empty())
      Out += " excl " + unionText(D.Excl);
    Out += "\n";
  }

  // A memory group of Times elements of Size bytes, Step apart, listing
  // exactly Seq.
  std::optional<std::string> stridedMemGroup(ArrayRef<Loc> Seq) const {
    size_t Size = 1;
    while (Size < Seq.size() && follows(Seq[Size - 1], Seq[Size]))
      ++Size;
    if (Size == Seq.size())
      return groupText(Seq);
    u64 Step = Seq[Size].Addr.Const - Seq[0].Addr.Const;
    if (Seq[Size].Addr.Sum != Seq[0].Addr.Sum || Seq.size() % Size ||
        Step >= (u64(1) << 32))
      return std::nullopt;
    size_t Times = Seq.size() / Size;
    for (size_t I = 0; I < Times; ++I)
      for (size_t K = 0; K < Size; ++K) {
        const Loc &L = Seq[I * Size + K];
        if (L.Addr.Sum != Seq[0].Addr.Sum ||
            L.Addr.Const != Seq[0].Addr.Const + I * Step + K)
          return std::nullopt;
      }
    return "M[" + Pool.str(Seq[0].Addr) + " .. +" + utostr(Size) +
           " step " + utostr(Step) + " times " + utostr(Times) + "]";
  }

  // The group that lists exactly Seq, in order: a register, a V range or a
  // (strided) memory group.
  std::optional<std::string> groupFor(ArrayRef<Loc> Seq) const {
    bool All = true;
    for (size_t I = 1; I < Seq.size(); ++I)
      All &= follows(Seq[I - 1], Seq[I]);
    if (All)
      return Seq[0].K == Loc::Reg && Seq.size() > 1
                 ? std::nullopt
                 : std::optional<std::string>(groupText(Seq));
    for (const Loc &L : Seq)
      if (L.K != Loc::Mem)
        return std::nullopt;
    return stridedMemGroup(Seq);
  }

  // A part as text: the same locations for every element (a union), or
  // element i made of the i-th element of each group ("per In:Out").
  std::optional<std::string> perPart(ArrayRef<const LocSet *> Sets,
                                     unsigned EOut) const {
    bool Same = true;
    for (const LocSet *S : Sets)
      Same &= *S == *Sets[0];
    if (Same)
      return unionText(*Sets[0]);
    std::vector<std::vector<Run>> Runs;
    for (const LocSet *S : Sets)
      Runs.push_back(consecutiveRuns(sorted(*S)));
    if (Runs[0].empty())
      return std::nullopt;
    size_t EIn = Runs[0][0].size();
    for (const auto &Element : Runs) {
      if (Element.size() != Runs[0].size())
        return std::nullopt;
      for (const Run &R : Element)
        if (R.size() != EIn)
          return std::nullopt;
    }
    std::string R;
    for (size_t J = 0; J < Runs[0].size(); ++J) {
      std::vector<Loc> Seq;
      for (const auto &Element : Runs)
        Seq.insert(Seq.end(), Element[J].begin(), Element[J].end());
      std::optional<std::string> G = groupFor(Seq);
      if (!G)
        return std::nullopt;
      R += (R.empty() ? "" : " | ") + *G;
    }
    R += " per " + utostr(EIn);
    if (EOut != EIn)
      R += ":" + utostr(EOut);
    return R;
  }

  // Element i from element i of each source: "V0[0:15] <- V1[0:15] |
  // V2[0:15] per 4".
  bool perLine(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    for (unsigned EOut : {1u, 2u, 4u, 8u}) {
      size_t Count = R.size() / EOut;
      if (R.size() % EOut || Count < 2)
        continue;
      bool Uniform = true;
      for (size_t I = 0; I < R.size() && Uniform; ++I)
        Uniform = *Deps[I] == *Deps[I / EOut * EOut];
      if (!Uniform)
        continue;
      std::vector<const LocSet *> Srcs, Excl;
      for (size_t I = 0; I < Count; ++I) {
        Srcs.push_back(&Deps[I * EOut]->Srcs);
        Excl.push_back(&Deps[I * EOut]->Excl);
      }
      std::optional<std::string> S = perPart(Srcs, EOut);
      std::optional<std::string> E = perPart(Excl, EOut);
      if (S && E) {
        Out += groupText(R) + " <- " + *S +
               (*E == "0" ? "" : " excl " + *E) + "\n";
        return true;
      }
    }
    return false;
  }

  static bool isClear(const Dep *D) {
    return D->Srcs.empty() && D->Excl.empty();
  }

  // Location i of R from the single V byte of Deps[i], or none (0):
  // "V0[0:15] <- [V1[3:15] V2[0:2]]".
  bool byteMapLine(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    for (const Dep *D : Deps)
      if (D->Srcs.size() > 1 || !D->Excl.empty() ||
          (D->Srcs.size() == 1 && D->Srcs.begin()->K != Loc::VByte))
        return false;
    std::string Items;
    std::vector<Loc> Pending;
    auto Flush = [&] {
      for (const Run &Rn : consecutiveRuns(Pending))
        Items += (Items.empty() ? "" : " ") + groupText(Rn);
      Pending.clear();
    };
    for (const Dep *D : Deps) {
      if (!D->Srcs.empty()) {
        Pending.push_back(*D->Srcs.begin());
        continue;
      }
      Flush();
      Items += (Items.empty() ? "" : " ") + std::string("0");
    }
    Flush();
    Out += groupText(R) + " <- [" + Items + "]\n";
    return true;
  }

  // The clear top of a whole V register from byte 1, 2, 4 or 8, on its own
  // line below the rest.
  bool clearTop(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    if (R[0].K != Loc::VByte || R.size() != 16)
      return false;
    for (size_t K : {1, 2, 4, 8}) {
      if (!all_of(Deps.drop_front(K), isClear))
        continue;
      if (all_of(Deps.take_front(K), isClear))
        return false;
      runLines(R.take_front(K), Deps.take_front(K));
      unionLine(R.drop_front(K), *Deps[K]);
      return true;
    }
    return false;
  }

  // Memory written from bytes of several V registers, one byte each: a line
  // per register, each listing its bytes in order, by the address of their
  // first byte.
  bool perRegister(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    if (R[0].K != Loc::Mem)
      return false;
    std::map<unsigned, std::vector<std::pair<Loc, Loc>>> ByReg; // V, dst, src
    std::vector<unsigned> Order;
    for (size_t I = 0; I < R.size(); ++I) {
      const Dep *D = Deps[I];
      if (D->Srcs.size() != 1 || D->Srcs.begin()->K != Loc::VByte ||
          !D->Excl.empty())
        return false;
      const Loc &S = *D->Srcs.begin();
      if (!ByReg.count(S.N))
        Order.push_back(S.N);
      ByReg[S.N].push_back({R[I], S});
    }
    if (Order.size() < 2)
      return false;
    std::vector<std::string> Lines;
    for (unsigned N : Order) {
      auto &Bytes = ByReg[N];
      std::stable_sort(Bytes.begin(), Bytes.end(),
                       [](const auto &A, const auto &B) {
                         return A.second.B < B.second.B;
                       });
      std::vector<Loc> Dsts, Srcs;
      for (const auto &[D, S] : Bytes) {
        Dsts.push_back(D);
        Srcs.push_back(S);
      }
      std::optional<std::string> To = groupFor(Dsts), From = groupFor(Srcs);
      if (!To || !From)
        return false;
      Lines.push_back(*To + " <- " + *From + " per 1\n");
    }
    for (const std::string &L : Lines)
      Out += L;
    return true;
  }

  // The registers (and memory bases) a dependency reads.
  static std::set<std::pair<unsigned, unsigned>> sources(const Dep *D) {
    std::set<std::pair<unsigned, unsigned>> R;
    for (const LocSet *S : {&D->Srcs, &D->Excl})
      for (const Loc &L : *S)
        R.insert({L.K, L.K == Loc::Mem ? L.Addr.Sum : L.N});
    return R;
  }

  // The run cut where the registers read change, when it gives several
  // parts.
  bool bySource(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    std::vector<size_t> Cuts;
    for (size_t I = 1; I < R.size(); ++I)
      if (sources(Deps[I]) != sources(Deps[I - 1]))
        Cuts.push_back(I);
    if (Cuts.empty())
      return false;
    Cuts.push_back(R.size());
    size_t Start = 0;
    for (size_t End : Cuts) {
      runLines(R.slice(Start, End - Start), Deps.slice(Start, End - Start));
      Start = End;
    }
    return true;
  }

  // The statements of the run R: one line when a shape fits the whole run,
  // otherwise the run cut further.
  void runLines(ArrayRef<Loc> R, ArrayRef<const Dep *> Deps) {
    if (all_of(Deps, [&](const Dep *D) { return *D == *Deps[0]; }))
      return unionLine(R, *Deps[0]);
    if (perLine(R, Deps) || clearTop(R, Deps) || perRegister(R, Deps) ||
        (R[0].K == Loc::VByte && byteMapLine(R, Deps)) || bySource(R, Deps))
      return;
    // Runs of equal dependencies, or of single sources that follow each other
    // as their locations do.
    auto Single = [&](size_t I) {
      return Deps[I]->Srcs.size() == 1 && Deps[I]->Excl.empty();
    };
    auto Next = [&](size_t I) {
      return Single(I - 1) && Single(I) &&
             follows(*Deps[I - 1]->Srcs.begin(), *Deps[I]->Srcs.begin());
    };
    size_t Start = 0;
    for (size_t I = 1; I <= R.size(); ++I) {
      bool Equal = *Deps[I - 1] == *Deps[Start];
      if (I < R.size() && (I - Start == 1 ? *Deps[I] == *Deps[Start] || Next(I)
                                          : Equal ? *Deps[I] == *Deps[Start]
                                                  : Next(I)))
        continue;
      ArrayRef<Loc> Dsts = R.slice(Start, I - Start);
      if (Equal) {
        unionLine(Dsts, *Deps[Start]);
      } else {
        LocSet Srcs;
        for (size_t K = Start; K < I; ++K)
          Srcs.insert(*Deps[K]->Srcs.begin());
        Out += groupText(Dsts) + " <- " + unionText(Srcs) + " per 1\n";
      }
      Start = I;
    }
  }

  // The sends, the tag statements (memory, then X registers and SP, then V
  // bytes), the tracked values, the opaque statement.
  void print(const Transfer &T) {
    for (uint8_t N : T.Sends)
      Out += "send X" + utostr(N) + "\n";
    std::vector<std::pair<Loc, const Dep *>> Written;
    for (const auto &[L, D] : T.Deps)
      Written.push_back({L, &D});
    std::sort(Written.begin(), Written.end(),
              [&](const auto &A, const auto &B) {
                return less(A.first, B.first);
              });
    for (size_t Start = 0, I = 1; I <= Written.size(); ++I)
      if (I == Written.size() ||
          !follows(Written[I - 1].first, Written[I].first)) {
        std::vector<Loc> R;
        std::vector<const Dep *> Deps;
        for (size_t K = Start; K < I; ++K) {
          R.push_back(Written[K].first);
          Deps.push_back(Written[K].second);
        }
        runLines(R, Deps);
        Start = I;
      }
    if (T.SP)
      Out += "<SP> <- " + Pool.str(*T.SP) + "\n";
    if (T.FP)
      Out += "<FP> <- " + Pool.str(*T.FP) + "\n";
    if (T.Opaque == Transfer::Svc)
      Out += "svc\n";
    else if (T.Opaque == Transfer::Unsupported)
      Out += "unsupported \"" + T.Reason + "\"\n";
  }
};
} // namespace

std::string llvm::hbng::printTransfer(const TermPool &Pool,
                                      const Transfer &T) {
  std::string Out;
  Printer(Pool, Out).print(T);
  return Out;
}
