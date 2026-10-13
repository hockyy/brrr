// Symbolic layer: unknowns and expressions as DATA, facts as constraints, queries
// answered mathematically with tri-state results (Yes / No / Unknown).
//
// This is what lets BridgeIR answer "do these two symbols have the same value?"
// as a core service instead of per-pass C++ analysis.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bridgeir::symbolic {

struct ExprTerm;
using Expr = std::shared_ptr<const ExprTerm>;

// v0 expression grammar: constants, unknowns, sums, products.
// (floor/mod syntax is deferred; alignment is reasoned about via facts, not syntax.)
struct ExprTerm {
  enum class Kind { Const, Var, Add, Mul };
  Kind kind = Kind::Const;
  int64_t c = 0;        // Kind::Const
  std::string name;     // Kind::Var
  std::vector<Expr> args;

  static Expr constant(int64_t c);
  static Expr var(std::string name);
  static Expr add(std::vector<Expr> xs);
  static Expr mul(std::vector<Expr> xs);
};

std::string pretty(const Expr& e);
bool structurallyEqual(const Expr& a, const Expr& b);

enum class Tri { Yes, No, Unknown };
const char* toString(Tri t);

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

  // Apply asserted equalities — used to hand fact-substituted expressions to
  // connectors.
  Expr subst(const Expr& e) const;

 private:
  struct Interval {
    std::optional<int64_t> lo, hi;
  };

  std::map<std::string, Expr> substMap() const;
  Interval evalInterval(const Expr& e) const;
  std::optional<int64_t> residueOf(const Expr& e, int64_t m) const;

  std::vector<std::pair<Expr, Expr>> equalities_;
  std::map<std::string, std::pair<int64_t, int64_t>> ranges_;
  std::vector<std::tuple<std::string, int64_t, int64_t>> mods_;
};

}  // namespace bridgeir::symbolic
