#include "symbolic/symbolic.h"

#include <algorithm>
#include <cstdlib>

namespace bridgeir::symbolic {
namespace {

using I128 = __int128;

std::optional<int64_t> safeAdd(std::optional<int64_t> a,
                               std::optional<int64_t> b) {
  if (!a || !b) return std::nullopt;
  const I128 r = static_cast<I128>(*a) + static_cast<I128>(*b);
  if (r < INT64_MIN || r > INT64_MAX) return std::nullopt;
  return static_cast<int64_t>(r);
}

std::optional<int64_t> safeMul(std::optional<int64_t> a,
                               std::optional<int64_t> b) {
  if (!a || !b) return std::nullopt;
  const I128 r = static_cast<I128>(*a) * static_cast<I128>(*b);
  if (r < INT64_MIN || r > INT64_MAX) return std::nullopt;
  return static_cast<int64_t>(r);
}

int64_t normMod(int64_t x, int64_t m) { return ((x % m) + m) % m; }

// Linear canonical form: c + sum(coeff * var). Complete for the linear fragment;
// nullopt means outside it (e.g. var * var).
struct LinForm {
  int64_t c = 0;
  std::map<std::string, int64_t> coeffs;
  bool operator==(const LinForm& o) const {
    return c == o.c && coeffs == o.coeffs;
  }
};

void dropZeroCoeffs(LinForm& f) {
  for (auto it = f.coeffs.begin(); it != f.coeffs.end();) {
    if (it->second == 0)
      it = f.coeffs.erase(it);
    else
      ++it;
  }
}

std::optional<LinForm> linOf(const Expr& e) {
  switch (e->kind) {
    case ExprTerm::Kind::Const: {
      LinForm f;
      f.c = e->c;
      return f;
    }
    case ExprTerm::Kind::Var: {
      LinForm f;
      f.coeffs[e->name] = 1;
      return f;
    }
    case ExprTerm::Kind::Add: {
      LinForm acc;
      for (const auto& a : e->args) {
        auto f = linOf(a);
        if (!f) return std::nullopt;
        acc.c += f->c;
        for (const auto& [k, v] : f->coeffs) acc.coeffs[k] += v;
      }
      dropZeroCoeffs(acc);
      return acc;
    }
    case ExprTerm::Kind::Mul: {
      int64_t const_prod = 1;
      std::optional<LinForm> nonconst;
      for (const auto& a : e->args) {
        if (a->kind == ExprTerm::Kind::Const) {
          const_prod *= a->c;
          continue;
        }
        auto f = linOf(a);
        if (!f) return std::nullopt;
        if (nonconst) return std::nullopt;  // two symbolic factors: out of fragment
        nonconst = *f;
      }
      if (!nonconst) {
        LinForm f;
        f.c = const_prod;
        return f;
      }
      LinForm f = *nonconst;
      f.c *= const_prod;
      for (auto& [k, v] : f.coeffs) v *= const_prod;
      dropZeroCoeffs(f);
      return f;
    }
  }
  return std::nullopt;
}

constexpr int kSubstDepth = 16;

Expr substRec(const Expr& e, const std::map<std::string, Expr>& m, int depth) {
  if (depth <= 0 || e->kind == ExprTerm::Kind::Const) return e;
  if (e->kind == ExprTerm::Kind::Var) {
    auto it = m.find(e->name);
    if (it == m.end()) return e;
    return substRec(it->second, m, depth - 1);
  }
  std::vector<Expr> args;
  args.reserve(e->args.size());
  for (const auto& a : e->args) args.push_back(substRec(a, m, depth));
  return e->kind == ExprTerm::Kind::Add ? ExprTerm::add(std::move(args))
                                        : ExprTerm::mul(std::move(args));
}

}  // namespace

Expr ExprTerm::constant(int64_t c) {
  auto e = std::make_shared<ExprTerm>();
  e->kind = Kind::Const;
  e->c = c;
  return e;
}

Expr ExprTerm::var(std::string name) {
  auto e = std::make_shared<ExprTerm>();
  e->kind = Kind::Var;
  e->name = std::move(name);
  return e;
}

Expr ExprTerm::add(std::vector<Expr> xs) {
  auto e = std::make_shared<ExprTerm>();
  e->kind = Kind::Add;
  e->args = std::move(xs);
  return e;
}

Expr ExprTerm::mul(std::vector<Expr> xs) {
  auto e = std::make_shared<ExprTerm>();
  e->kind = Kind::Mul;
  e->args = std::move(xs);
  return e;
}

std::string pretty(const Expr& e) {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return std::to_string(e->c);
    case ExprTerm::Kind::Var:
      return e->name;
    default: {
      const char* sep = e->kind == ExprTerm::Kind::Add ? " + " : " * ";
      std::string out = "(";
      for (size_t i = 0; i < e->args.size(); ++i) {
        if (i) out += sep;
        out += pretty(e->args[i]);
      }
      out += ")";
      return out;
    }
  }
}

bool structurallyEqual(const Expr& a, const Expr& b) {
  if (a->kind != b->kind) return false;
  if (a->kind == ExprTerm::Kind::Const) return a->c == b->c;
  if (a->kind == ExprTerm::Kind::Var) return a->name == b->name;
  if (a->args.size() != b->args.size()) return false;
  for (size_t i = 0; i < a->args.size(); ++i) {
    if (!structurallyEqual(a->args[i], b->args[i])) return false;
  }
  return true;
}

const char* toString(Tri t) {
  switch (t) {
    case Tri::Yes: return "Yes";
    case Tri::No: return "No";
    case Tri::Unknown: return "Unknown";
  }
  return "?";
}

void Solver::assertEqual(const Expr& a, const Expr& b) {
  equalities_.emplace_back(a, b);
}

void Solver::assertRange(const std::string& var, int64_t lo, int64_t hi) {
  ranges_[var] = {lo, hi};
}

void Solver::assertMod(const std::string& var, int64_t modulus,
                       int64_t residue) {
  mods_.emplace_back(var, modulus, residue);
}

std::map<std::string, Expr> Solver::substMap() const {
  std::map<std::string, Expr> m;
  for (const auto& [a, b] : equalities_) {
    if (a->kind == ExprTerm::Kind::Var) m[a->name] = b;
    if (b->kind == ExprTerm::Kind::Var) m[b->name] = a;
  }
  return m;
}

Expr Solver::subst(const Expr& e) const {
  return substRec(e, substMap(), kSubstDepth);
}

Solver::Interval Solver::evalInterval(const Expr& e) const {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return {e->c, e->c};
    case ExprTerm::Kind::Var: {
      auto it = ranges_.find(e->name);
      if (it == ranges_.end()) return {std::nullopt, std::nullopt};
      return {it->second.first, it->second.second};
    }
    case ExprTerm::Kind::Add: {
      Interval acc{0, 0};
      for (const auto& a : e->args) {
        const Interval ia = evalInterval(a);
        acc.lo = safeAdd(acc.lo, ia.lo);
        acc.hi = safeAdd(acc.hi, ia.hi);
      }
      return acc;
    }
    case ExprTerm::Kind::Mul: {
      Interval acc{1, 1};
      for (const auto& a : e->args) {
        const Interval ia = evalInterval(a);
        std::vector<int64_t> prods;
        for (const auto x : {acc.lo, acc.hi}) {
          for (const auto y : {ia.lo, ia.hi}) {
            if (auto p = safeMul(x, y)) prods.push_back(*p);
          }
        }
        if (prods.empty()) return {std::nullopt, std::nullopt};
        acc = {*std::min_element(prods.begin(), prods.end()),
               *std::max_element(prods.begin(), prods.end())};
      }
      return acc;
    }
  }
  return {std::nullopt, std::nullopt};
}

std::optional<int64_t> Solver::residueOf(const Expr& e, int64_t m) const {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return normMod(e->c, m);
    case ExprTerm::Kind::Var: {
      std::optional<int64_t> found;
      for (const auto& [v, mm, rr] : mods_) {
        if (v != e->name) continue;
        if (mm % m == 0) {  // var = rr (mod mm), m | mm  =>  var = rr (mod m)
          const int64_t r = normMod(rr, m);
          if (found && *found != r) return std::nullopt;  // conflicting facts
          found = r;
        }
      }
      return found;
    }
    case ExprTerm::Kind::Add: {
      int64_t acc = 0;
      for (const auto& a : e->args) {
        auto r = residueOf(a, m);
        if (!r) return std::nullopt;
        acc += *r;
      }
      return normMod(acc, m);
    }
    case ExprTerm::Kind::Mul: {
      int64_t const_prod = 1;
      bool any_const = false;
      for (const auto& a : e->args) {
        if (a->kind == ExprTerm::Kind::Const) {
          const_prod = normMod(const_prod * normMod(a->c, m), m);
          any_const = true;
        }
      }
      if (any_const && const_prod == 0) return int64_t{0};  // e.g. 4 * i  (mod 4)
      int64_t acc = any_const ? const_prod : 1;
      for (const auto& a : e->args) {
        if (a->kind == ExprTerm::Kind::Const) continue;
        auto r = residueOf(a, m);
        if (!r) return std::nullopt;
        acc = normMod(acc * *r, m);
      }
      return acc;
    }
  }
  return std::nullopt;
}

Tri Solver::isEqual(const Expr& a, const Expr& b) const {
  const Expr a2 = subst(a);
  const Expr b2 = subst(b);
  const auto la = linOf(a2);
  const auto lb = linOf(b2);
  if (la && lb) {
    if (*la == *lb) return Tri::Yes;
    LinForm d;
    d.c = la->c - lb->c;
    d.coeffs = la->coeffs;
    for (const auto& [k, v] : lb->coeffs) d.coeffs[k] -= v;
    dropZeroCoeffs(d);
    // A nonzero constant difference proves inequality; a difference containing
    // unknowns could still vanish, so the honest answer is Unknown.
    if (d.coeffs.empty()) return d.c == 0 ? Tri::Yes : Tri::No;
    return Tri::Unknown;
  }
  return structurallyEqual(a2, b2) ? Tri::Yes : Tri::Unknown;
}

Tri Solver::isLess(const Expr& a, const Expr& b) const {
  const Interval ia = evalInterval(subst(a));
  const Interval ib = evalInterval(subst(b));
  if (ia.hi && ib.lo && *ia.hi < *ib.lo) return Tri::Yes;
  if (ia.lo && ib.hi && *ia.lo >= *ib.hi) return Tri::No;
  return Tri::Unknown;
}

Tri Solver::aligned(const Expr& e, int64_t modulus) const {
  if (modulus <= 0) return Tri::Unknown;
  const auto r = residueOf(subst(e), modulus);
  if (!r) return Tri::Unknown;
  return *r == 0 ? Tri::Yes : Tri::No;
}

std::pair<std::optional<int64_t>, std::optional<int64_t>> Solver::bounds(
    const Expr& e) const {
  const Interval i = evalInterval(subst(e));
  return {i.lo, i.hi};
}

}  // namespace bridgeir::symbolic
