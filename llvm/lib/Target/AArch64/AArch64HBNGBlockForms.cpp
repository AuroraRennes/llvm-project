//===-- AArch64HBNGBlockForms.cpp - HBNG block forms ----------------------===//
//
// The groups of a block composed, left to right, into forms.
//
//===----------------------------------------------------------------------===//

#include "AArch64HBNGBlockForms.h"
#include <array>
#include <cassert>
#include <iterator>

using namespace llvm;
using namespace llvm::hbng;

namespace {
constexpr unsigned NoSend = ~0u;
using Pairing = std::array<unsigned, 31>; // per X register, a channel entry

// Which send gives each register its value, so a form can name it: for each
// group, and each Xn, the number of the block's send whose value Xn holds when
// the group starts (i for S<i>), or NoSend. In
//   str x8, [x16]     S0
//   add x8, x8, #1
//   str x8, [x16]     S1
// x8 is S0 at the add, and S1 after it.
std::vector<Pairing> pairSends(ArrayRef<Group> Groups) {
  size_t N = Groups.size();
  std::vector<Pairing> Pair(N);
  // The block's channel entries, numbered in order: group K's first is
  // FirstEntry[K].
  std::vector<unsigned> FirstEntry(N + 1, 0);
  for (size_t K = 0; K < N; ++K)
    FirstEntry[K + 1] = FirstEntry[K] + Groups[K].T.Sends.size();

  // First, the nearest preceding send of the register, the group's own
  // included, with no write of it in between.
  Pairing Last;
  Last.fill(NoSend);
  std::array<size_t, 31> LastGroup{};
  for (size_t K = 0; K < N; ++K) {
    const Group &G = Groups[K];
    for (size_t I = 0; I < G.T.Sends.size(); ++I) {
      Last[G.T.Sends[I]] = FirstEntry[K] + I;
      LastGroup[G.T.Sends[I]] = K;
    }
    Pair[K] = Last;
    // A write ends the register's send, except the sending group's own: it
    // comes before the send (a store-exclusive sends its status).
    for (unsigned R = 0; R < 31; ++R)
      if ((G.Writes >> R & 1) && Last[R] != NoSend && LastGroup[R] != K)
        Last[R] = NoSend;
  }

  // Otherwise, the nearest following send, with no write from the group on.
  Pairing Next;
  Next.fill(NoSend);
  for (size_t K = N; K-- > 0;) {
    const Group &G = Groups[K];
    for (unsigned R = 0; R < 31; ++R) {
      if (G.Writes >> R & 1)
        Next[R] = NoSend;
      if (Pair[K][R] == NoSend)
        Pair[K][R] = Next[R];
    }
    for (size_t I = G.T.Sends.size(); I-- > 0;)
      Next[G.T.Sends[I]] = FirstEntry[K] + I;
  }
  return Pair;
}

// Rewrites memory locations' addresses with Map, once per sum: the bytes of an
// access share theirs.
class Rewriter {
  TermPool &Pool;
  function_ref<std::optional<Term>(Term)> Map;
  DenseMap<unsigned, Term> Done;

public:
  Rewriter(TermPool &Pool, function_ref<std::optional<Term>(Term)> Map)
      : Pool(Pool), Map(Map) {}
  Term operator()(Term A) {
    auto [It, New] = Done.try_emplace(A.Sum);
    if (New)
      It->second = Pool.subst({A.Sum, 0}, Map);
    return Pool.plus(It->second, int64_t(A.Const));
  }
  Loc operator()(const Loc &L) {
    return L.K == Loc::Mem ? Loc::mem((*this)(L.Addr)) : L;
  }
};

bool isIdentity(const Loc &L, const Dep &D) {
  return D.Srcs.size() == 1 && *D.Srcs.begin() == L && D.Excl.empty();
}

// Whether Deps writes a memory byte whose address has another sum than S.
// Memory bytes come first in Deps, ordered by sum.
bool writesOtherSum(const std::map<Loc, Dep> &Deps, unsigned S) {
  if (Deps.empty() || Deps.begin()->first.K != Loc::Mem)
    return false;
  if (Deps.begin()->first.Addr.Sum != S)
    return true;
  return std::prev(Deps.lower_bound(Loc::x(0)))->first.Addr.Sum != S;
}

// Adds what L's tag is made of at F's exit to Srcs and Excl (to Excl alone
// without Srcs); false when F may or may not have written L: a byte is
// written by F only if F wrote a byte of the same sum (equal constants), and
// not if F wrote bytes of its sum only.
bool addAtExit(const Transfer &F, const Loc &L, LocSet *Srcs, LocSet &Excl) {
  LocSet &Into = Srcs ? *Srcs : Excl;
  auto It = F.Deps.find(L);
  if (It == F.Deps.end()) {
    if (L.K == Loc::Mem && writesOtherSum(F.Deps, L.Addr.Sum))
      return false;
    Into.insert(L);
    return true;
  }
  Into.insert(It->second.Srcs.begin(), It->second.Srcs.end());
  Excl.insert(It->second.Excl.begin(), It->second.Excl.end());
  return true;
}
} // namespace

BlockForms::BlockForms(TermPool &Pool)
    : Pool(Pool), SP(Pool.sp()), FP(Pool.fp()), LO12(Pool.lo12()) {
  for (unsigned N = 0; N < 31; ++N)
    RegOf[Pool.reg(N).Sum] = N;
}

// F followed by Next, in place; false, F unchanged, when an address Next
// reads or writes cannot be told apart from one F wrote, or when two
// locations Next writes are the same once F's tracked values are put in.
bool BlockForms::then(Transfer &F, const Transfer &Next) {
  auto Entry = [&](Term S) -> std::optional<Term> {
    if (S == SP)
      return F.SP;
    if (S == FP)
      return F.FP;
    return std::nullopt;
  };
  bool Moved = F.SP || F.FP;
  Rewriter Rewrite(Pool, Entry);
  auto Rebase = [&](const Loc &L) { return Moved ? Rewrite(L) : L; };

  std::map<Loc, Dep> New;
  for (const auto &[L, D] : Next.Deps) {
    Dep ND;
    for (const Loc &S : D.Srcs)
      if (!addAtExit(F, Rebase(S), &ND.Srcs, ND.Excl))
        return false;
    for (const Loc &S : D.Excl)
      if (!addAtExit(F, Rebase(S), nullptr, ND.Excl))
        return false;
    if (!New.emplace(Rebase(L), std::move(ND)).second)
      return false;
  }

  // A byte F wrote and Next doesn't overwrite must be told apart from each
  // byte Next writes.
  if (!New.empty() && New.begin()->first.K == Loc::Mem) {
    unsigned S = New.begin()->first.Addr.Sum;
    if (!writesOtherSum(New, S)) {
      if (writesOtherSum(F.Deps, S))
        return false;
    } else {
      for (const auto &[L, D] : F.Deps)
        if (L.K != Loc::Mem)
          break;
        else if (!New.count(L))
          return false;
    }
  }

  auto Value = [&](const std::optional<Term> &Own, std::optional<Term> &Out,
                   Term Symbol) {
    if (Own)
      Out = Pool.subst(*Own, Entry);
    if (Out && *Out == Symbol)
      Out.reset();
  };
  std::optional<Term> NewSP = F.SP, NewFP = F.FP;
  Value(Next.SP, NewSP, SP);
  Value(Next.FP, NewFP, FP);
  F.SP = NewSP;
  F.FP = NewFP;

  for (auto &[L, D] : New) {
    F.Deps.erase(L);
    if (!isIdentity(L, D))
      F.Deps.emplace(L, std::move(D));
  }
  F.Sends.append(Next.Sends.begin(), Next.Sends.end());
  return true;
}

std::optional<std::vector<Form>> BlockForms::compose(ArrayRef<Group> Groups) {
  std::vector<Pairing> Pair = pairSends(Groups);
  std::vector<Form> Forms;
  bool Open = false;
  for (size_t K = 0; K < Groups.size(); ++K) {
    unsigned Offset = 4 * K;
    // The names that hold across the block: <Xn> its send, LO12 its
    // instruction's.
    bool Unpaired = false;
    auto Names = [&](Term S) -> std::optional<Term> {
      if (S == LO12)
        return Pool.lo12At(Offset);
      auto It = RegOf.find(S.Sum);
      if (It == RegOf.end())
        return std::nullopt;
      unsigned Entry = Pair[K][It->second];
      if (Entry == NoSend) {
        Unpaired = true;
        return std::nullopt;
      }
      return Pool.send(Entry);
    };
    Rewriter Rename(Pool, Names);
    const Transfer &In = Groups[K].T;
    Transfer Own;
    for (const auto &[L, D] : In.Deps) {
      Dep ND;
      for (const Loc &S : D.Srcs)
        ND.Srcs.insert(Rename(S));
      for (const Loc &S : D.Excl)
        ND.Excl.insert(Rename(S));
      if (!Own.Deps.emplace(Rename(L), std::move(ND)).second)
        return std::nullopt;
    }
    if (In.SP)
      Own.SP = Rename(*In.SP);
    if (In.FP)
      Own.FP = Rename(*In.FP);
    Own.Sends = In.Sends;
    Own.Opaque = In.Opaque;
    Own.Reason = In.Reason;
    if (Unpaired)
      return std::nullopt;

    // svc and unsupported stand alone.
    if (Own.isOpaque()) {
      Forms.push_back({Offset, std::move(Own)});
      Open = false;
      continue;
    }
    if (Open && then(Forms.back().T, Own))
      continue;
    Forms.push_back({Offset, Transfer()});
    [[maybe_unused]] bool Composed = then(Forms.back().T, Own);
    assert(Composed && "HBNG: a transfer that does not compose alone");
    Open = true;
  }
  return Forms;
}
