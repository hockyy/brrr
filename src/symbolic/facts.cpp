#include "symbolic/facts.h"

#include <cctype>
#include <cstdlib>

#include "symbolic/expr_io.h"

namespace brrr::symbolic {
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

bool isAtom(const Expr& e) { return e->kind == ExprTerm::Kind::Var; }

}  // namespace

bool parseFact(const std::string& text, ParsedFact* out, std::string* err) {
  std::string body = trim(text);
  if (body.rfind("fact", 0) == 0 &&
      (body.size() == 4 || std::isspace(static_cast<unsigned char>(body[4])))) {
    body = trim(body.substr(4));
  }
  if (body.empty()) {
    *err = "empty fact";
    return false;
  }
  *out = ParsedFact{};

  // Functor forms: aligned(e, m) / range(e, lo, hi).
  const bool aligned = body.rfind("aligned(", 0) == 0;
  const bool range = body.rfind("range(", 0) == 0;
  if (aligned || range) {
    const size_t lp = body.find('(');
    const size_t rp = body.rfind(')');
    if (rp == std::string::npos || rp != body.size() - 1) {
      *err = "malformed fact: expected ')' at the end";
      return false;
    }
    const std::vector<std::string> args =
        splitTopLevel(body.substr(lp + 1, rp - lp - 1), ',');
    if (args.size() != (aligned ? 2u : 3u)) {
      *err = aligned ? "aligned expects (atom, modulus)" : "range expects (atom, lo, hi)";
      return false;
    }
    Expr e = parseExpr(args[0], err);
    if (!e) return false;
    if (!isAtom(e)) {
      *err = std::string(aligned ? "aligned" : "range") +
             " needs a value or symbol as its first argument";
      return false;
    }
    out->kind = aligned ? ParsedFact::Kind::Aligned : ParsedFact::Kind::Range;
    out->exprs.push_back(e);
    if (aligned) {
      if (!parseIntStrict(args[1], &out->a) || out->a <= 0) {
        *err = "aligned modulus must be a positive integer";
        return false;
      }
    } else if (!parseIntStrict(args[1], &out->a) ||
               !parseIntStrict(args[2], &out->b) || out->a > out->b) {
      *err = "range bounds must be integers with lo <= hi";
      return false;
    }
    return true;
  }

  // Relations, possibly chained: `0 <= %i <= %n` is `0 <= %i` and `%i <= %n`.
  // Strict `<`/`>` are rejected loudly rather than silently mis-parsed.
  std::vector<std::string> sides;
  std::string cur;
  int depth = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (depth == 0 && (c == '<' || c == '>' || c == '=')) {
      const char nx = i + 1 < body.size() ? body[i + 1] : '\0';
      std::string op(1, c);
      if (nx == '=') {
        op += '=';
        ++i;
      } else if (c != '=') {
        *err = "strict inequalities are not supported; use `<=` or `>=` in: " + body;
        return false;
      }
      sides.push_back(trim(cur));
      out->ops.push_back(op == "==" ? "=" : op);
      cur.clear();
      continue;
    }
    cur += c;
  }
  sides.push_back(trim(cur));
  if (out->ops.empty()) {
    *err = "unrecognized fact (expected a relation, aligned(...), or range(...))";
    return false;
  }
  for (const std::string& side : sides) {
    Expr e = parseExpr(side, err);
    if (!e) return false;
    out->exprs.push_back(e);
  }
  return true;
}

std::string printFact(const ParsedFact& f,
                      const std::function<std::string(const std::string&)>& rename) {
  switch (f.kind) {
    case ParsedFact::Kind::Aligned:
      return "aligned(" + printExpr(f.exprs[0], rename) + ", " + std::to_string(f.a) + ")";
    case ParsedFact::Kind::Range:
      return "range(" + printExpr(f.exprs[0], rename) + ", " + std::to_string(f.a) +
             ", " + std::to_string(f.b) + ")";
    case ParsedFact::Kind::Relation: {
      std::string s = printExpr(f.exprs[0], rename);
      for (size_t k = 0; k < f.ops.size(); ++k) {
        s += " " + f.ops[k] + " " + printExpr(f.exprs[k + 1], rename);
      }
      return s;
    }
  }
  return {};
}

ParsedFact mapFactVars(const ParsedFact& f,
                       const std::function<std::string(const std::string&)>& rename) {
  ParsedFact out = f;
  for (Expr& e : out.exprs) e = mapVars(e, rename);
  return out;
}

void collectFactVars(const ParsedFact& f, std::set<std::string>* out) {
  for (const Expr& e : f.exprs) collectVars(e, out);
}

bool applyFact(const std::string& text, Solver& s, std::string* err) {
  ParsedFact f;
  if (!parseFact(text, &f, err)) return false;
  const std::string shown = printFact(f);

  if (f.kind == ParsedFact::Kind::Aligned) {
    if (s.aligned(f.exprs[0], f.a) == Tri::No) {
      *err = "fact contradicts existing facts: " + shown + " is refuted";
      return false;
    }
    s.assertMod(f.exprs[0]->name, f.a, 0);
    return true;
  }
  if (f.kind == ParsedFact::Kind::Range) {
    const std::string& v = f.exprs[0]->name;
    int64_t lo = f.a, hi = f.b;
    if (auto cur = s.rangeOf(v)) {
      lo = std::max(lo, cur->first);
      hi = std::min(hi, cur->second);
      if (lo > hi) {
        *err = "fact contradicts existing range for '" + v + "'";
        return false;
      }
    }
    s.assertRange(v, lo, hi);
    return true;
  }
  for (size_t k = 0; k < f.ops.size(); ++k) {
    const Expr& l = f.exprs[k];
    const Expr& r = f.exprs[k + 1];
    if (f.ops[k] == "=") {
      if (s.isEqual(l, r) == Tri::No) {
        *err = "fact contradicts existing facts: " + shown + " is refuted";
        return false;
      }
      s.assertEqual(l, r);
      continue;
    }
    const Expr& lo = f.ops[k] == "<=" ? l : r;  // a >= b  ==  b <= a
    const Expr& hi = f.ops[k] == "<=" ? r : l;
    if (s.lessOrEqual(lo, hi) == Tri::No) {
      *err = "fact contradicts existing facts: " + shown + " is refuted";
      return false;
    }
    s.assertLe(lo, hi);
  }
  return true;
}

namespace {

// Apply every fact on the chain root -> scope, in document order per scope.
bool applyScopeChain(const brrr::Module& m, brrr::NodeId scope,
                     Solver* s, std::string* err) {
  std::vector<brrr::NodeId> chain;
  for (brrr::NodeId x = scope; x != brrr::kInvalidId;) {
    chain.insert(chain.begin(), x);
    const brrr::Node* n = m.find(x);
    x = n ? n->scope : brrr::kInvalidId;
  }
  chain.insert(chain.begin(), brrr::kInvalidId);
  for (const brrr::NodeId sc : chain) {
    for (const brrr::Module::FactEntry& f : m.facts) {
      if (f.scope == sc && !applyFact(f.text, *s, err)) return false;
    }
  }
  return true;
}

}  // namespace

bool validateAllFacts(const brrr::Module& m, std::string* err) {
  std::set<brrr::NodeId> scopes{brrr::kInvalidId};
  for (const brrr::Module::FactEntry& f : m.facts) scopes.insert(f.scope);
  for (const brrr::NodeId sc : scopes) {
    Solver s;
    std::string local;
    if (!applyScopeChain(m, sc, &s, &local)) {
      *err = (sc == brrr::kInvalidId ? std::string("root")
                                         : "scope n" + std::to_string(sc)) +
             ": " + local;
      return false;
    }
  }
  return true;
}

Solver solverForScope(const brrr::Module& m, brrr::NodeId scope,
                      std::string* err) {
  Solver s;
  std::string local;
  if (!applyScopeChain(m, scope, &s, &local) && err) *err = local;
  return s;
}

}  // namespace brrr::symbolic
