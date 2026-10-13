#include "symbolic/facts.h"

#include <cctype>
#include <cstdlib>

#include "symbolic/expr_io.h"

namespace bridgeir::symbolic {
namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

bool parseIntStrict(const std::string& s, int64_t* out) {
  const std::string t = trim(s);
  if (t.empty()) return false;
  size_t start = (t[0] == '-' || t[0] == '+') ? 1 : 0;
  if (start >= t.size()) return false;
  for (size_t i = start; i < t.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
  }
  *out = std::strtoll(t.c_str(), nullptr, 10);
  return true;
}

}  // namespace

bool applyFact(const std::string& line, Solver& s, std::string* err) {
  const std::string t = trim(line);
  if (t.rfind("fact", 0) != 0) {
    *err = "expected a 'fact' line";
    return false;
  }
  const std::string body = trim(t.substr(4));
  if (body.empty()) {
    *err = "empty fact";
    return false;
  }

  const size_t lp = body.find('(');
  if (lp != std::string::npos) {
    // Functor form: aligned(e, m) / range(v, lo, hi)
    const size_t rp = body.rfind(')');
    if (rp == std::string::npos || rp < lp) {
      *err = "malformed fact: missing ')'";
      return false;
    }
    const std::string name = trim(body.substr(0, lp));
    const std::vector<std::string> args =
        splitTopLevel(body.substr(lp + 1, rp - lp - 1), ',');

    if (name == "aligned") {
      if (args.size() != 2) {
        *err = "aligned expects (symbol, modulus)";
        return false;
      }
      Expr e = parseExpr(args[0], err);
      if (!e) return false;
      if (e->kind != ExprTerm::Kind::Var) {
        *err = "aligned requires a symbol on the left for now";
        return false;
      }
      int64_t m = 0;
      if (!parseIntStrict(args[1], &m) || m <= 0) {
        *err = "aligned modulus must be a positive integer";
        return false;
      }
      if (s.aligned(e, m) == Tri::No) {
        *err = "fact contradicts existing facts: aligned(" + args[0] + ", " +
               std::to_string(m) + ") is refuted";
        return false;
      }
      s.assertMod(e->name, m, 0);
      return true;
    }

    if (name == "range") {
      if (args.size() != 3) {
        *err = "range expects (symbol, lo, hi)";
        return false;
      }
      Expr e = parseExpr(args[0], err);
      if (!e) return false;
      if (e->kind != ExprTerm::Kind::Var) {
        *err = "range requires a symbol as its first argument";
        return false;
      }
      int64_t lo = 0, hi = 0;
      if (!parseIntStrict(args[1], &lo) || !parseIntStrict(args[2], &hi) ||
          lo > hi) {
        *err = "range bounds must be integers with lo <= hi";
        return false;
      }
      if (auto cur = s.rangeOf(e->name)) {
        const int64_t nlo = std::max(lo, cur->first);
        const int64_t nhi = std::min(hi, cur->second);
        if (nlo > nhi) {
          *err = "fact contradicts existing range for '" + e->name + "'";
          return false;
        }
        s.assertRange(e->name, nlo, nhi);
      } else {
        s.assertRange(e->name, lo, hi);
      }
      return true;
    }

    *err = "unknown fact '" + name + "'";
    return false;
  }

  // Inequality form: expr <= expr (strict `<` is not supported; use `<=`).
  const size_t le = body.find("<=");
  if (le != std::string::npos) {
    Expr lhs = parseExpr(trim(body.substr(0, le)), err);
    if (!lhs) return false;
    Expr rhs = parseExpr(trim(body.substr(le + 2)), err);
    if (!rhs) return false;
    if (s.lessOrEqual(rhs, lhs) == Tri::No) {
      *err = "fact contradicts existing facts: " + trim(body) + " is refuted";
      return false;
    }
    s.assertLe(lhs, rhs);
    return true;
  }

  // Equality form: expr = expr
  const size_t eq = body.find('=');
  if (eq == std::string::npos) {
    *err = "unrecognized fact (expected '=' or a known functor)";
    return false;
  }
  Expr lhs = parseExpr(trim(body.substr(0, eq)), err);
  if (!lhs) return false;
  Expr rhs = parseExpr(trim(body.substr(eq + 1)), err);
  if (!rhs) return false;
  if (s.isEqual(lhs, rhs) == Tri::No) {
    *err = "fact contradicts existing facts: " + trim(body) + " is refuted";
    return false;
  }
  s.assertEqual(lhs, rhs);
  return true;
}

bool applyFacts(const std::vector<std::string>& lines, Solver& s,
                std::string* err) {
  for (const std::string& line : lines) {
    if (!applyFact(line, s, err)) return false;
  }
  return true;
}

namespace {

// Apply every fact on the chain root -> scope, in document order per scope.
bool applyScopeChain(const bridgeir::Module& m, bridgeir::NodeId scope,
                     Solver* s, std::string* err) {
  std::vector<bridgeir::NodeId> chain;
  for (bridgeir::NodeId x = scope; x != bridgeir::kInvalidId;) {
    chain.push_back(x);
    const bridgeir::Node* n = m.find(x);
    x = n ? n->scope : bridgeir::kInvalidId;
  }
  for (size_t i = 0, j = chain.size(); i + 1 < j; ++i, --j) {
    std::swap(chain[i], chain[j - 1]);
  }
  auto applyScope = [&](bridgeir::NodeId sc) {
    for (const bridgeir::Module::FactEntry& f : m.facts) {
      if (f.scope == sc) {
        if (!applyFact(f.line, *s, err)) return false;
      }
    }
    return true;
  };
  if (!applyScope(bridgeir::kInvalidId)) return false;
  for (const bridgeir::NodeId x : chain) {
    if (!applyScope(x)) return false;
  }
  return true;
}

}  // namespace

bool validateAllFacts(const bridgeir::Module& m, std::string* err) {
  std::vector<bridgeir::NodeId> scopes;
  scopes.push_back(bridgeir::kInvalidId);
  for (const bridgeir::Module::FactEntry& f : m.facts) {
    if (f.scope == bridgeir::kInvalidId) continue;
    bool seen = false;
    for (const bridgeir::NodeId x : scopes) {
      if (x == f.scope) {
        seen = true;
        break;
      }
    }
    if (!seen) scopes.push_back(f.scope);
  }
  for (const bridgeir::NodeId sc : scopes) {
    Solver s;
    std::string local;
    if (!applyScopeChain(m, sc, &s, &local)) {
      *err = (sc == bridgeir::kInvalidId ? std::string("root")
                                         : "scope n" + std::to_string(sc)) +
             ": " + local;
      return false;
    }
  }
  return true;
}

Solver solverForScope(const bridgeir::Module& m, bridgeir::NodeId scope,
                      std::string* err) {
  Solver s;
  std::string local;
  if (!applyScopeChain(m, scope, &s, &local)) {
    if (err) *err = local;
  }
  return s;
}

}  // namespace bridgeir::symbolic
