// Symbolic layer: unknowns and expressions as DATA, facts as constraints, queries
// answered mathematically with tri-state results (Yes / No / Unknown).
//
// This is what lets Brrr answer "do these two symbols have the same value?"
// as a core service instead of per-pass C++ analysis.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace brrr::symbolic {

struct ExprTerm;
using Expr = std::shared_ptr<const ExprTerm>;

// Expressions: constants, atoms, sums, products, and division by a positive
// constant (floordiv rounds toward -inf, ceildiv toward +inf, mod is in [0, c)).
// Subtraction is a sum with a -1 factor. Atom names: `%7` (a value, by id),
// `tile` (a module symbol), `?%7.0` (dim(%7, 0)). A division's divisor is
// args[1], always a positive Const.
struct ExprTerm {
  enum class Kind { Const, Var, Add, Mul, FloorDiv, CeilDiv, Mod };
  Kind kind = Kind::Const;
  int64_t c = 0;        // Kind::Const
  std::string name;     // Kind::Var
  std::vector<Expr> args;

  static Expr constant(int64_t c);
  static Expr var(std::string name);
  static Expr add(std::vector<Expr> xs);
  static Expr mul(std::vector<Expr> xs);
  static Expr div(Kind kind, Expr e, int64_t c);  // FloorDiv | CeilDiv | Mod
  // The same kind of node over new arguments (used by every rewrite).
  static Expr rebuild(const Expr& like, std::vector<Expr> args);
  bool isDivision() const {
    return kind == Kind::FloorDiv || kind == Kind::CeilDiv || kind == Kind::Mod;
  }
};

std::string pretty(const Expr& e);
bool structurallyEqual(const Expr& a, const Expr& b);

// `e` as c + sum(coeff * atom), when it is linear (degree <= 1 after expanding).
bool linearize(const Expr& e, int64_t* c, std::map<std::string, int64_t>* coeffs);

enum class Tri { Yes, No, Unknown };
const char* toString(Tri t);

namespace z3 {
class Session;
}

// Who decides what the fast paths leave open (BRRR_SOLVER=z3|local|check):
//   z3     the local tiers, then Z3 for what they leave open (the default when
//          Brrr was built with Z3)
//   local  the local tiers alone: the rules, then exact linear arithmetic
//   check  both; Z3's answer is used, and any disagreement is printed to
//          stderr as "solver check:" — the way to audit the fallback
enum class Backend { Local, Z3, Check };
Backend backend();
const char* backendName();
size_t solverDisagreements();

// Which tier decided each question, in the order they are tried:
//   theorem   equal normal forms (Yes) or a nonzero constant difference (No)
//   local     the local rules: substitution, linear facts, intervals, residues,
//             sums of non-negative monomials
//   exact     linear integer arithmetic, decided exactly (presburger.h); divisions
//             by constants become variables, nonlinear monomials opaque atoms
//   z3        Z3 (linear questions on the QF_LIA solver; see z3::stats())
//   replayed  a recorded Z3 answer (AnswerCache)
// Every tier is sound, so the first Yes or No wins. BRRR_SOLVER_STATS=1 prints these.
struct TierStats {
  uint64_t theorem = 0, local = 0, exact = 0, z3 = 0, replayed = 0, unknown = 0;
};
const TierStats& tierStats();

// Recorded answers. With Z3, an answer is a function of its question key (Z3
// version, rlimit, the facts in order, the question; see z3_backend.h), so a run
// store can record answers and replay them: a branch or a resumed run gets exactly
// the answers the original got. Only the Z3 backend records; `check` re-asks.
class AnswerCache {
 public:
  // `question` is the full key text; entries are stored by its 128-bit hash.
  std::optional<std::string> find(const std::string& question);
  void put(const std::string& question, const std::string& answer);
  bool load(const std::string& path);       // a missing file is an empty cache
  bool appendNew(const std::string& path);  // writes what was put since the last append
  size_t replayed() const { return replayed_; }
  size_t asked() const { return asked_; }

 private:
  std::map<std::string, std::string> answers_;  // hash -> answer
  std::vector<std::pair<std::string, std::string>> fresh_;
  size_t replayed_ = 0, asked_ = 0;
};
void setAnswerCache(AnswerCache* cache);  // nullptr: answers are not recorded

// Facts, and Yes/No/Unknown questions about them. Two fast paths are theorems,
// not searches, and always run first: equal polynomial normal forms (Yes) and a
// nonzero constant difference (No). Everything else goes to the backend.
class Solver {
 public:
  // Facts (constraints asserted by schemas, importers, or agents).
  void assertEqual(const Expr& a, const Expr& b);
  void assertRange(const std::string& var, int64_t lo, int64_t hi);
  void assertMod(const std::string& var, int64_t modulus, int64_t residue);

  // Queries. Unknown is a first-class answer: honest, never guessed.
  Tri isEqual(const Expr& a, const Expr& b) const;
  Tri isLess(const Expr& a, const Expr& b) const;
  Tri aligned(const Expr& e, int64_t modulus) const;
  std::pair<std::optional<int64_t>, std::optional<int64_t>> bounds(
      const Expr& e) const;
  std::optional<std::pair<int64_t, int64_t>> rangeOf(
      const std::string& var) const;

  // Inequality facts: a <= b (used by scoped invariants).
  void assertLe(const Expr& a, const Expr& b);
  Tri lessOrEqual(const Expr& a, const Expr& b) const;

  // Apply asserted equalities — used to hand fact-substituted expressions to
  // connectors.
  Expr subst(const Expr& e) const;

  // The value of `e` when the facts pin it to a single integer.
  std::optional<int64_t> constantValue(const Expr& e) const;

  // The local tiers alone (the rules, then exact linear arithmetic), never Z3:
  // for tests and audits. rel: '=' '<' 'l' (<=) 'a' (aligned to `modulus`);
  // `settled` is set when an Unknown is final (see ruleTiers).
  Tri localTiers(char rel, const Expr& a, const Expr& b, int64_t modulus = 0,
                 bool* settled = nullptr) const {
    return ruleTiers(rel, a, b, modulus, settled);
  }

 private:
  struct Interval {
    std::optional<int64_t> lo, hi;
  };

  // The local fallback (see Backend).
  Tri localIsEqual(const Expr& a, const Expr& b) const;
  Tri localIsLess(const Expr& a, const Expr& b) const;
  Tri localAligned(const Expr& e, int64_t modulus) const;
  Tri localLessOrEqual(const Expr& a, const Expr& b) const;
  std::optional<int64_t> localConstantValue(const Expr& e) const;
  // The local tiers for one question: the rules, then exact linear arithmetic.
  // rel: '=' (a == b), 'l' (a <= b), '<' (a < b), 'a' (a aligned to `modulus`).
  // `settled` is set when an Unknown is final: the exact tier saw the whole
  // problem (no fact dropped, no nonlinear atom, budget left) and found the
  // question satisfiable both ways, so no solver can answer Yes or No.
  Tri ruleTiers(char rel, const Expr& a, const Expr& b, int64_t modulus,
                bool* settled = nullptr) const;
  Tri exactLinear(char rel, const Expr& a, const Expr& b, int64_t modulus, bool* settled) const;

  // This solver's facts in Z3, built on first use. A copy starts without one:
  // copies add their own facts (e.g. a branch condition), so they cannot share.
  struct LazySession {
    std::unique_ptr<z3::Session> p;
    LazySession();
    ~LazySession();
    LazySession(const LazySession&);
    LazySession& operator=(const LazySession&);
  };
  z3::Session& session() const;
  // `check` mode: report a Yes/No disagreement, or a settled Unknown that Z3
  // decides (the exact tier claimed no solver could).
  Tri decide(Tri local, bool settled, Tri z3, const char* what, const Expr& a,
             const Expr& b) const;
  // A Z3 answer, replayed from the AnswerCache when it holds the question.
  std::string recorded(const std::string& question, const std::function<std::string()>& ask) const;
  Tri recordedTri(const std::string& question, const std::function<Tri()>& ask) const;

  // Equalities oriented into `atom -> expression` with no atom on its own right
  // side and no mapped atom on any right side, so one pass substitutes fully.
  const std::map<std::string, Expr>& substMap() const;
  Interval evalInterval(const Expr& e) const;
  std::optional<int64_t> residueOf(const Expr& e, int64_t m) const;

  std::vector<std::pair<Expr, Expr>> equalities_;
  std::vector<std::pair<Expr, Expr>> le_facts_;
  std::map<std::string, std::pair<int64_t, int64_t>> ranges_;
  std::vector<std::tuple<std::string, int64_t, int64_t>> mods_;
  mutable std::map<std::string, Expr> subst_map_;
  mutable std::vector<std::pair<Expr, Expr>> residual_;  // equalities not oriented
  mutable bool subst_valid_ = false;
  mutable LazySession z3_;  // dropped on every new fact, so Z3 sees facts in canonical order
  mutable std::string facts_key_;
  mutable bool facts_key_valid_ = false;
};

}  // namespace brrr::symbolic
