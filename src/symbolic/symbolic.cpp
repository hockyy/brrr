#include "symbolic/symbolic.h"

#include "symbolic/z3_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace brrr::symbolic {
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

// Polynomial canonical form: monomial (sorted atom multiset) -> coefficient.
// Complete for equality of polynomials; nullopt on overflow or blow-up.
using Monomial = std::vector<std::string>;
using Poly = std::map<Monomial, int64_t>;
constexpr size_t kMaxPolyTerms = 256;

void dropZeros(Poly& p) {
  for (auto it = p.begin(); it != p.end();) {
    it = it->second == 0 ? p.erase(it) : std::next(it);
  }
}

std::optional<Poly> polyOf(const Expr& e);

std::optional<LinForm> linOf(const Expr& e) {
  if (e->isDivision()) {
    const auto p = polyOf(e);
    if (!p) return std::nullopt;
    LinForm f;
    for (const auto& [mono, k] : *p) {
      if (mono.empty()) f.c = k;
      else if (mono.size() == 1) f.coeffs[mono[0]] = k;
      else return std::nullopt;
    }
    return f;
  }
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
    default:
      break;
  }
  return std::nullopt;
}

// One substitution pass. The map is kept fully substituted (see substMap), so a
// single pass is complete and cannot loop.
Expr applyMap(const Expr& e, const std::map<std::string, Expr>& m) {
  if (e->kind == ExprTerm::Kind::Const) return e;
  if (e->kind == ExprTerm::Kind::Var) {
    auto it = m.find(e->name);
    return it == m.end() ? e : it->second;
  }
  std::vector<Expr> args;
  args.reserve(e->args.size());
  for (const auto& a : e->args) args.push_back(applyMap(a, m));
  return ExprTerm::rebuild(e, std::move(args));
}

bool occurs(const std::string& var, const Expr& e) {
  if (e->kind == ExprTerm::Kind::Var) return e->name == var;
  for (const auto& a : e->args) {
    if (occurs(var, a)) return true;
  }
  return false;
}

Expr fromLin(const LinForm& f) {
  std::vector<Expr> terms;
  for (const auto& [k, v] : f.coeffs) {
    terms.push_back(v == 1 ? ExprTerm::var(k)
                           : ExprTerm::mul({ExprTerm::constant(v), ExprTerm::var(k)}));
  }
  if (f.c != 0 || terms.empty()) terms.push_back(ExprTerm::constant(f.c));
  return terms.size() == 1 ? terms[0] : ExprTerm::add(std::move(terms));
}

int64_t floorDiv(int64_t a, int64_t c) { return a / c - ((a % c != 0) && ((a < 0) != (c < 0))); }
int64_t ceilDiv(int64_t a, int64_t c) { return -floorDiv(-a, c); }

// A deterministic spelling of a polynomial, to name an opaque term after it.
std::string polyText(const Poly& p) {
  std::string s;
  for (const auto& [mono, c] : p) {
    s += std::to_string(c);
    for (const std::string& a : mono) s += "*" + a;
    s += ";";
  }
  return s;
}

// e floordiv c, e ceildiv c, e mod c. Exact when the inner polynomial is
// divisible by c (or constant); otherwise one opaque atom, floordiv(inner', c),
// named after the inner polynomial so equal divisions are the same atom:
//   ceildiv(e, c) = floordiv(e + c - 1, c)     mod(e, c) = e - c * floordiv(e, c)
std::optional<Poly> divisionPoly(const Expr& e) {
  const int64_t c = e->args[1]->c;
  auto inner = polyOf(e->args[0]);
  if (!inner || c <= 0) return std::nullopt;
  const bool constant = inner->empty() || (inner->size() == 1 && inner->begin()->first.empty());
  if (constant) {
    const int64_t v = inner->empty() ? 0 : inner->begin()->second;
    const int64_t r = e->kind == ExprTerm::Kind::FloorDiv  ? floorDiv(v, c)
                      : e->kind == ExprTerm::Kind::CeilDiv ? ceilDiv(v, c)
                                                           : v - c * floorDiv(v, c);
    Poly p;
    if (r != 0) p[{}] = r;
    return p;
  }
  bool divisible = true;
  for (const auto& [mono, k] : *inner) divisible = divisible && k % c == 0;
  if (divisible) {
    if (e->kind == ExprTerm::Kind::Mod) return Poly{};
    Poly q;
    for (const auto& [mono, k] : *inner) q[mono] = k / c;
    return q;
  }
  Poly arg = *inner;
  if (e->kind == ExprTerm::Kind::CeilDiv) {
    arg[{}] += c - 1;
    dropZeros(arg);
  }
  const std::string atom = "{floordiv " + polyText(arg) + " by " + std::to_string(c) + "}";
  if (e->kind != ExprTerm::Kind::Mod) return Poly{{{atom}, 1}};
  Poly m = *inner;  // e - c * q
  auto s = safeAdd(m[{atom}], -c);
  if (!s) return std::nullopt;
  m[{atom}] = *s;
  dropZeros(m);
  return m;
}

// Every non-exact floordiv term in `e`, with the bounds that define it:
//   q = floordiv(x, c)   =>   c * q <= x   and   x <= c * q + c - 1
// (ceildiv(x, c) is floordiv(x + c - 1, c); mod is defined through floordiv.)
void divisionBounds(const Expr& e, std::vector<std::pair<Expr, Expr>>* out);

std::optional<Poly> polyOf(const Expr& e) {
  if (e->isDivision()) return divisionPoly(e);
  switch (e->kind) {
    case ExprTerm::Kind::Const: {
      Poly p;
      if (e->c != 0) p[{}] = e->c;
      return p;
    }
    case ExprTerm::Kind::Var:
      return Poly{{{e->name}, 1}};
    case ExprTerm::Kind::Add: {
      Poly acc;
      for (const auto& a : e->args) {
        auto p = polyOf(a);
        if (!p) return std::nullopt;
        for (const auto& [m, c] : *p) {
          auto s = safeAdd(acc[m], c);
          if (!s) return std::nullopt;
          acc[m] = *s;
        }
      }
      dropZeros(acc);
      return acc;
    }
    case ExprTerm::Kind::Mul: {
      Poly acc{{{}, 1}};
      for (const auto& a : e->args) {
        auto p = polyOf(a);
        if (!p) return std::nullopt;
        Poly next;
        for (const auto& [m1, c1] : acc) {
          for (const auto& [m2, c2] : *p) {
            Monomial m = m1;
            m.insert(m.end(), m2.begin(), m2.end());
            std::sort(m.begin(), m.end());
            auto prod = safeMul(c1, c2);
            if (!prod) return std::nullopt;
            auto s = safeAdd(next[m], *prod);
            if (!s) return std::nullopt;
            next[m] = *s;
          }
        }
        dropZeros(next);
        if (next.size() > kMaxPolyTerms) return std::nullopt;
        acc = std::move(next);
      }
      return acc;
    }
    default:  // divisions are handled before the switch
      break;
  }
  return std::nullopt;
}

void divisionBounds(const Expr& e, std::vector<std::pair<Expr, Expr>>* out) {
  for (const Expr& a : e->args) divisionBounds(a, out);
  if (!e->isDivision()) return;
  const int64_t c = e->args[1]->c;
  Expr x = e->args[0];
  if (e->kind == ExprTerm::Kind::CeilDiv) x = ExprTerm::add({x, ExprTerm::constant(c - 1)});
  const auto p = polyOf(ExprTerm::div(ExprTerm::Kind::FloorDiv, x, c));
  if (!p || p->size() != 1 || p->begin()->first.size() != 1 || p->begin()->second != 1 ||
      p->begin()->first[0].rfind("{floordiv", 0) != 0) {
    return;  // an exact division needs no bounds
  }
  const Expr cq = ExprTerm::mul({ExprTerm::constant(c), ExprTerm::var(p->begin()->first[0])});
  out->emplace_back(cq, x);
  out->emplace_back(x, ExprTerm::add({cq, ExprTerm::constant(c - 1)}));
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

Expr ExprTerm::div(Kind kind, Expr x, int64_t c) {
  auto e = std::make_shared<ExprTerm>();
  e->kind = kind;
  e->args = {std::move(x), constant(c)};
  return e;
}

Expr ExprTerm::rebuild(const Expr& like, std::vector<Expr> args) {
  switch (like->kind) {
    case Kind::Add: return add(std::move(args));
    case Kind::Mul: return mul(std::move(args));
    case Kind::FloorDiv:
    case Kind::CeilDiv:
    case Kind::Mod: {
      auto e = std::make_shared<ExprTerm>();
      e->kind = like->kind;
      e->args = std::move(args);
      return e;
    }
    default: return like;
  }
}

std::string pretty(const Expr& e) {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return std::to_string(e->c);
    case ExprTerm::Kind::Var:
      return e->name;
    case ExprTerm::Kind::FloorDiv:
    case ExprTerm::Kind::CeilDiv:
    case ExprTerm::Kind::Mod: {
      const char* op = e->kind == ExprTerm::Kind::FloorDiv  ? " floordiv "
                       : e->kind == ExprTerm::Kind::CeilDiv ? " ceildiv "
                                                            : " mod ";
      return "(" + pretty(e->args[0]) + op + pretty(e->args[1]) + ")";
    }
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

bool linearize(const Expr& e, int64_t* c, std::map<std::string, int64_t>* coeffs) {
  const auto p = polyOf(e);  // expands products first: (a + 1) * 2 is linear
  if (!p) return false;
  *c = 0;
  coeffs->clear();
  for (const auto& [mono, k] : *p) {
    if (mono.empty()) {
      *c = k;
    } else if (mono.size() == 1) {
      (*coeffs)[mono[0]] = k;
    } else {
      return false;
    }
  }
  return true;
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

std::optional<std::pair<int64_t, int64_t>> Solver::rangeOf(
    const std::string& var) const {
  auto it = ranges_.find(var);
  if (it == ranges_.end()) return std::nullopt;
  return it->second;
}

// Orient each equality into `atom -> expression`, in assertion order:
//   1. a bare atom on either side (`%9 = 2 * %2`, `tile = 128`, `%9 = %2`), or
//   2. a linear equation with a unit-coefficient atom, solved for it
//      (`%m + 1 = %n`  ->  `%m -> %n - 1`).
// An atom is never mapped to an expression containing itself, and every right
// side is rewritten when a new atom is mapped, so the map stays acyclic and one
// substitution pass is complete. Equalities that cannot be oriented (e.g.
// `a * b = c * d`) are kept but only checked, never substituted.
const std::map<std::string, Expr>& Solver::substMap() const {
  if (subst_valid_) return subst_map_;
  subst_map_.clear();
  residual_.clear();
  auto bind = [&](const std::string& var, const Expr& rhs) {
    const std::map<std::string, Expr> one{{var, rhs}};
    for (auto& [k, v] : subst_map_) v = applyMap(v, one);
    subst_map_[var] = rhs;
  };
  for (const auto& [a, b] : equalities_) {
    const Expr a2 = applyMap(a, subst_map_);
    const Expr b2 = applyMap(b, subst_map_);
    if (a2->kind == ExprTerm::Kind::Var && !occurs(a2->name, b2)) {
      bind(a2->name, b2);
      continue;
    }
    if (b2->kind == ExprTerm::Kind::Var && !occurs(b2->name, a2)) {
      bind(b2->name, a2);
      continue;
    }
    const auto la = linOf(a2);
    const auto lb = linOf(b2);
    if (!la || !lb) {
      residual_.emplace_back(a, b);
      continue;
    }
    LinForm d = *la;  // d == 0
    d.c -= lb->c;
    for (const auto& [k, v] : lb->coeffs) d.coeffs[k] -= v;
    dropZeroCoeffs(d);
    for (const auto& [k, v] : d.coeffs) {
      if (v != 1 && v != -1) continue;
      // v*k + rest == 0  ->  k == -rest / v  ==  -v * rest  (v is +-1)
      LinForm rest = d;
      rest.coeffs.erase(k);
      rest.c *= -v;
      for (auto& [rk, rv] : rest.coeffs) rv *= -v;
      bind(k, fromLin(rest));
      break;
    }
  }
  subst_valid_ = true;
  return subst_map_;
}

Expr Solver::subst(const Expr& e) const { return applyMap(e, substMap()); }

std::optional<int64_t> Solver::localConstantValue(const Expr& e) const {
  const auto p = polyOf(subst(e));
  if (!p) return std::nullopt;
  if (p->empty()) return int64_t{0};
  if (p->size() == 1 && p->begin()->first.empty()) return p->begin()->second;
  return std::nullopt;
}

Solver::Interval Solver::evalInterval(const Expr& e) const {
  if (e->isDivision()) {
    const int64_t c = e->args[1]->c;
    const Interval in = evalInterval(e->args[0]);
    if (e->kind == ExprTerm::Kind::Mod) {
      if (in.lo && in.hi && floorDiv(*in.lo, c) == floorDiv(*in.hi, c)) {
        return {*in.lo - c * floorDiv(*in.lo, c), *in.hi - c * floorDiv(*in.hi, c)};
      }
      return {int64_t{0}, c - 1};
    }
    auto f = e->kind == ExprTerm::Kind::FloorDiv ? floorDiv : ceilDiv;
    Interval out;
    if (in.lo) out.lo = f(*in.lo, c);
    if (in.hi) out.hi = f(*in.hi, c);
    return out;
  }
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return {e->c, e->c};
    case ExprTerm::Kind::Var: {
      // A range fact, tightened by constant bounds among the <= facts
      // (`0 <= %n` gives %n a lower bound of 0).
      Interval out;
      if (auto it = ranges_.find(e->name); it != ranges_.end()) {
        out = {it->second.first, it->second.second};
      }
      for (const auto& [lo, hi] : le_facts_) {
        if (lo->kind == ExprTerm::Kind::Const && hi->kind == ExprTerm::Kind::Var &&
            hi->name == e->name && (!out.lo || lo->c > *out.lo)) {
          out.lo = lo->c;
        }
        if (hi->kind == ExprTerm::Kind::Const && lo->kind == ExprTerm::Kind::Var &&
            lo->name == e->name && (!out.hi || hi->c < *out.hi)) {
          out.hi = hi->c;
        }
      }
      return out;
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
      // Both sides bounded: all four corner products (any overflow: unknown).
      // Both sides non-negative: [lo * lo, hi * hi], hi unbounded if either is.
      // Anything else: unknown. (Skipping an undefined corner would be unsound.)
      Interval acc{1, 1};
      for (const auto& a : e->args) {
        const Interval ia = evalInterval(a);
        if (acc.lo && acc.hi && ia.lo && ia.hi) {
          std::vector<int64_t> prods;
          for (const auto x : {acc.lo, acc.hi}) {
            for (const auto y : {ia.lo, ia.hi}) {
              const auto p = safeMul(x, y);
              if (!p) return {std::nullopt, std::nullopt};
              prods.push_back(*p);
            }
          }
          acc = {*std::min_element(prods.begin(), prods.end()),
                 *std::max_element(prods.begin(), prods.end())};
        } else if (acc.lo && ia.lo && *acc.lo >= 0 && *ia.lo >= 0) {
          const auto lo = safeMul(acc.lo, ia.lo);
          if (!lo) return {std::nullopt, std::nullopt};
          acc = {lo, acc.hi && ia.hi ? safeMul(acc.hi, ia.hi) : std::nullopt};
        } else {
          return {std::nullopt, std::nullopt};
        }
      }
      return acc;
    }
    default:  // divisions are handled before the switch
      break;
  }
  return {std::nullopt, std::nullopt};
}

std::optional<int64_t> Solver::residueOf(const Expr& e, int64_t m) const {
  if (e->isDivision()) {
    // (x mod c) mod m == x mod m when m divides c; otherwise unknown.
    if (e->kind == ExprTerm::Kind::Mod && e->args[1]->c % m == 0) return residueOf(e->args[0], m);
    return std::nullopt;
  }
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
    default:  // divisions are handled before the switch
      break;
  }
  return std::nullopt;
}

// Equality decided on polynomial normal forms after substitution: a zero
// difference is Yes, a nonzero constant difference is No. A difference that still
// contains atoms is No only when its interval excludes zero; otherwise it could
// vanish, so the honest answer is Unknown.
Tri Solver::localIsEqual(const Expr& a, const Expr& b) const {
  const Expr a2 = subst(a);
  const Expr b2 = subst(b);
  const Expr diff = ExprTerm::add({a2, ExprTerm::mul({ExprTerm::constant(-1), b2})});
  if (const auto d = polyOf(diff)) {
    if (d->empty()) return Tri::Yes;
    if (d->size() == 1 && d->begin()->first.empty()) return Tri::No;
    // An equality that could not be oriented (`a * b = c + d`) still proves
    // exactly itself, up to sign.
    for (const auto& [l, r] : residual_) {
      const Expr rd = ExprTerm::add({subst(l), ExprTerm::mul({ExprTerm::constant(-1), subst(r)})});
      const auto p = polyOf(rd);
      if (!p) continue;
      if (*p == *d) return Tri::Yes;
      Poly neg = *p;
      for (auto& [mono, c] : neg) c = -c;
      if (neg == *d) return Tri::Yes;
    }
    const Interval iv = evalInterval(diff);
    if ((iv.lo && *iv.lo > 0) || (iv.hi && *iv.hi < 0)) return Tri::No;
    return Tri::Unknown;
  }
  return structurallyEqual(a2, b2) ? Tri::Yes : Tri::Unknown;
}

Tri Solver::localIsLess(const Expr& a, const Expr& b) const {
  const Interval ia = evalInterval(subst(a));
  const Interval ib = evalInterval(subst(b));
  if (ia.hi && ib.lo && *ia.hi < *ib.lo) return Tri::Yes;
  if (ia.lo && ib.hi && *ia.lo >= *ib.hi) return Tri::No;
  return Tri::Unknown;
}

Tri Solver::localAligned(const Expr& e, int64_t modulus) const {
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

// a <= b, decided from: constant differences, asserted <= facts with the same
// linear shape, and interval bounds. Unknown is honest.
Tri Solver::localLessOrEqual(const Expr& a, const Expr& b) const {
  const Expr a2 = subst(a);
  const Expr b2 = subst(b);
  const auto ca = linOf(a2);
  const auto cb = linOf(b2);

  if (ca && cb) {
    LinForm d = *ca;
    d.c -= cb->c;
    for (const auto& [k, v] : cb->coeffs) d.coeffs[k] -= v;
    dropZeroCoeffs(d);

    if (d.coeffs.empty()) return d.c <= 0 ? Tri::Yes : Tri::No;

    // Same-shape implication: if F <= 0 is a fact and D <= F, then D <= 0.
    // Division terms bring their defining bounds along.
    std::vector<std::pair<Expr, Expr>> facts = le_facts_;
    divisionBounds(a2, &facts);
    divisionBounds(b2, &facts);
    for (const auto& [fa, fb] : facts) {
      const auto cfa = linOf(subst(fa));
      const auto cfb = linOf(subst(fb));
      if (!cfa || !cfb) continue;
      LinForm f = *cfa;
      f.c -= cfb->c;
      for (const auto& [k, v] : cfb->coeffs) f.coeffs[k] -= v;
      dropZeroCoeffs(f);
      if (f.coeffs == d.coeffs && d.c <= f.c) return Tri::Yes;
    }

    const Interval la = evalInterval(a2);
    const Interval lb = evalInterval(b2);
    if (la.hi && lb.lo && *la.hi <= *lb.lo) return Tri::Yes;
    if (la.lo && lb.hi && *la.lo > *lb.hi) return Tri::No;
    return Tri::Unknown;
  }

  if (structurallyEqual(a2, b2)) return Tri::Yes;
  const Interval la = evalInterval(a2);
  const Interval lb = evalInterval(b2);
  if (la.hi && lb.lo && *la.hi <= *lb.lo) return Tri::Yes;
  if (la.lo && lb.hi && *la.lo > *lb.hi) return Tri::No;
  return Tri::Unknown;
}


// ---- backends -------------------------------------------------------------------

Backend backend() {
  static const Backend b = [] {
    const char* v = std::getenv("BRRR_SOLVER");
    const std::string want = v ? v : "";
    if (!z3::available()) return Backend::Local;
    if (want == "local") return Backend::Local;
    if (want == "check") return Backend::Check;
    return Backend::Z3;
  }();
  return b;
}

const char* backendName() {
  switch (backend()) {
    case Backend::Local: return "local";
    case Backend::Z3: return "z3";
    case Backend::Check: return "check";
  }
  return "?";
}

namespace {
size_t g_disagreements = 0;
AnswerCache* g_cache = nullptr;

// Two independent FNV-1a passes: a 128-bit name for a question.
std::string hashQuestion(const std::string& s) {
  uint64_t h1 = 1469598103934665603ull, h2 = 0x6c62272e07bb0142ull;
  for (const unsigned char c : s) {
    h1 = (h1 ^ c) * 1099511628211ull;
    h2 = (h2 ^ (c + 0x9eu)) * 0x100000001b3ull;
  }
  h2 ^= s.size();
  char buf[33];
  std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(h2));
  return buf;
}

const char* triText(Tri t) {
  return t == Tri::Yes ? "yes" : t == Tri::No ? "no" : "unknown";
}
Tri parseTri(const std::string& s) {
  return s == "yes" ? Tri::Yes : s == "no" ? Tri::No : Tri::Unknown;
}
}  // namespace

size_t solverDisagreements() { return g_disagreements; }

std::optional<std::string> AnswerCache::find(const std::string& question) {
  const auto it = answers_.find(hashQuestion(question));
  if (it == answers_.end()) return std::nullopt;
  ++replayed_;
  return it->second;
}

void AnswerCache::put(const std::string& question, const std::string& answer) {
  const std::string h = hashQuestion(question);
  answers_[h] = answer;
  fresh_.emplace_back(h, answer);
  ++asked_;
}

bool AnswerCache::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return true;
  std::string h, a;
  while (in >> h >> a) answers_[h] = a;
  return true;
}

bool AnswerCache::appendNew(const std::string& path) {
  if (fresh_.empty()) return true;
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return false;
  for (const auto& [h, a] : fresh_) out << h << " " << a << "\n";
  fresh_.clear();
  return static_cast<bool>(out);
}

void setAnswerCache(AnswerCache* cache) { g_cache = cache; }

std::string Solver::recorded(const std::string& question,
                             const std::function<std::string()>& ask) const {
  if (!g_cache || backend() != Backend::Z3) return ask();
  if (!facts_key_valid_) {
    std::string k;
    for (const auto& [a, b] : equalities_) k += "eq " + pretty(a) + " = " + pretty(b) + "\n";
    for (const auto& [a, b] : le_facts_) k += "le " + pretty(a) + " <= " + pretty(b) + "\n";
    for (const auto& [v, r] : ranges_) {
      k += "range " + v + " " + std::to_string(r.first) + " " + std::to_string(r.second) + "\n";
    }
    for (const auto& [v, m, r] : mods_) {
      k += "mod " + v + " " + std::to_string(m) + " " + std::to_string(r) + "\n";
    }
    facts_key_ = std::move(k);
    facts_key_valid_ = true;
  }
  const std::string key = z3::version() + " rlimit " + std::to_string(z3::rlimit()) + "\n" +
                          facts_key_ + "? " + question;
  if (auto hit = g_cache->find(key)) return *hit;
  std::string answer = ask();
  g_cache->put(key, answer);
  return answer;
}

Tri Solver::recordedTri(const std::string& question, const std::function<Tri()>& ask) const {
  return parseTri(recorded(question, [&] { return std::string(triText(ask())); }));
}

Solver::LazySession::LazySession() = default;
Solver::LazySession::~LazySession() = default;
Solver::LazySession::LazySession(const LazySession&) {}
Solver::LazySession& Solver::LazySession::operator=(const LazySession&) {
  p.reset();
  return *this;
}

z3::Session& Solver::session() const {
  if (!z3_.p) {
    z3_.p = std::make_unique<z3::Session>();
    for (const auto& [a, b] : equalities_) z3_.p->assertEqual(a, b);
    for (const auto& [a, b] : le_facts_) z3_.p->assertLe(a, b);
    for (const auto& [v, r] : ranges_) z3_.p->assertRange(v, r.first, r.second);
    for (const auto& [v, m, r] : mods_) z3_.p->assertMod(v, m, r);
  }
  return *z3_.p;
}

Tri Solver::decide(Tri local, Tri z3, const char* what, const Expr& a, const Expr& b) const {
  if (local != Tri::Unknown && z3 != Tri::Unknown && local != z3) {
    ++g_disagreements;
    std::fprintf(stderr, "solver check: %s %s %s -> local %s, z3 %s\n", pretty(a).c_str(), what,
                 b ? pretty(b).c_str() : "", toString(local), toString(z3));
  }
  return z3;
}

// ---- facts ------------------------------------------------------------------------

void Solver::assertEqual(const Expr& a, const Expr& b) {
  equalities_.emplace_back(a, b);
  subst_valid_ = false;
  z3_.p.reset();
  facts_key_valid_ = false;
}

void Solver::assertRange(const std::string& var, int64_t lo, int64_t hi) {
  ranges_[var] = {lo, hi};
  z3_.p.reset();
  facts_key_valid_ = false;
}

void Solver::assertMod(const std::string& var, int64_t modulus, int64_t residue) {
  mods_.emplace_back(var, modulus, residue);
  z3_.p.reset();
  facts_key_valid_ = false;
}

void Solver::assertLe(const Expr& a, const Expr& b) {
  le_facts_.emplace_back(a, b);
  z3_.p.reset();
  facts_key_valid_ = false;
}

// ---- questions --------------------------------------------------------------------

Tri Solver::isEqual(const Expr& a, const Expr& b) const {
  // Theorems first: equal normal forms, or a nonzero constant difference.
  const Expr diff = ExprTerm::add({subst(a), ExprTerm::mul({ExprTerm::constant(-1), subst(b)})});
  if (const auto d = polyOf(diff)) {
    if (d->empty()) return Tri::Yes;
    if (d->size() == 1 && d->begin()->first.empty()) return Tri::No;
  }
  switch (backend()) {
    case Backend::Local: return localIsEqual(a, b);
    case Backend::Z3:
      return recordedTri("== " + pretty(a) + " , " + pretty(b),
                         [&] { return session().isEqual(a, b); });
    case Backend::Check: return decide(localIsEqual(a, b), session().isEqual(a, b), "==", a, b);
  }
  return Tri::Unknown;
}

Tri Solver::lessOrEqual(const Expr& a, const Expr& b) const {
  const auto d = linOf(ExprTerm::add({subst(a), ExprTerm::mul({ExprTerm::constant(-1), subst(b)})}));
  if (d && d->coeffs.empty()) return d->c <= 0 ? Tri::Yes : Tri::No;
  switch (backend()) {
    case Backend::Local: return localLessOrEqual(a, b);
    case Backend::Z3:
      return recordedTri("<= " + pretty(a) + " , " + pretty(b),
                         [&] { return session().lessOrEqual(a, b); });
    case Backend::Check:
      return decide(localLessOrEqual(a, b), session().lessOrEqual(a, b), "<=", a, b);
  }
  return Tri::Unknown;
}

Tri Solver::isLess(const Expr& a, const Expr& b) const {
  const auto d = linOf(ExprTerm::add({subst(a), ExprTerm::mul({ExprTerm::constant(-1), subst(b)})}));
  if (d && d->coeffs.empty()) return d->c < 0 ? Tri::Yes : Tri::No;
  switch (backend()) {
    case Backend::Local: return localIsLess(a, b);
    case Backend::Z3:
      return recordedTri("< " + pretty(a) + " , " + pretty(b),
                         [&] { return session().isLess(a, b); });
    case Backend::Check: return decide(localIsLess(a, b), session().isLess(a, b), "<", a, b);
  }
  return Tri::Unknown;
}

Tri Solver::aligned(const Expr& e, int64_t modulus) const {
  if (modulus <= 0) return Tri::Unknown;
  if (const auto c = polyOf(subst(e)); c && (c->empty() || (c->size() == 1 && c->begin()->first.empty()))) {
    const int64_t v = c->empty() ? 0 : c->begin()->second;
    return normMod(v, modulus) == 0 ? Tri::Yes : Tri::No;
  }
  switch (backend()) {
    case Backend::Local: return localAligned(e, modulus);
    case Backend::Z3:
      return recordedTri("aligned " + pretty(e) + " , " + std::to_string(modulus),
                         [&] { return session().aligned(e, modulus); });
    case Backend::Check:
      return decide(localAligned(e, modulus), session().aligned(e, modulus), "aligned to",
                    e, ExprTerm::constant(modulus));
  }
  return Tri::Unknown;
}

std::optional<int64_t> Solver::constantValue(const Expr& e) const {
  if (const auto local = localConstantValue(e)) return local;  // a constant normal form
  if (backend() == Backend::Local) return std::nullopt;
  const std::string a = recorded("const " + pretty(e), [&] {
    const auto v = session().constantValue(e);
    return v ? std::to_string(*v) : std::string("none");
  });
  if (a == "none") return std::nullopt;
  return std::stoll(a);
}

}  // namespace brrr::symbolic
