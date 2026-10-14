#include "symbolic/presburger.h"

#include <algorithm>
#include <set>
#include <utility>

namespace brrr::symbolic::presburger {
namespace {

struct Overflow {};

// Coefficients stay well inside int64 so sums of a few products cannot wrap.
constexpr __int128 kLimit = static_cast<__int128>(1) << 62;

int64_t fit(__int128 v) {
  if (v > kLimit || v < -kLimit) throw Overflow{};
  return static_cast<int64_t>(v);
}

int64_t absval(int64_t a) { return a < 0 ? -a : a; }

int64_t gcd(int64_t a, int64_t b) {
  a = absval(a);
  b = absval(b);
  while (b) {
    const int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

int64_t floorDiv(int64_t a, int64_t b) {  // b > 0
  int64_t q = a / b;
  if (a % b != 0 && a < 0) --q;
  return q;
}

// The integer nearest a / b: remainders a - b*q stay within |b| / 2.
int64_t nearestDiv(int64_t a, int64_t b) {
  if (b < 0) {
    a = -a;
    b = -b;
  }
  return floorDiv(fit(static_cast<__int128>(a) + b / 2), b);
}

// k += f * (expr + ec), with `var` removed from k first (its coefficient is f).
void substitute(Constraint& k, const std::string& var, const Coeffs& expr, int64_t ec) {
  const auto it = k.coeffs.find(var);
  if (it == k.coeffs.end()) return;
  const int64_t f = it->second;
  k.coeffs.erase(it);
  for (const auto& [v, a] : expr) {
    k.coeffs[v] = fit(static_cast<__int128>(k.coeffs[v]) + static_cast<__int128>(f) * a);
  }
  k.c = fit(static_cast<__int128>(k.c) + static_cast<__int128>(f) * ec);
}

enum class Norm { Keep, Drop, Contradiction };

// Drop zero coefficients and divide by their gcd: an equality whose constant is
// not divisible has no integer solution; an inequality's constant rounds down.
Norm normalize(Constraint& k) {
  for (auto it = k.coeffs.begin(); it != k.coeffs.end();) {
    it = it->second == 0 ? k.coeffs.erase(it) : std::next(it);
  }
  if (k.coeffs.empty()) {
    if (k.eq) return k.c == 0 ? Norm::Drop : Norm::Contradiction;
    return k.c >= 0 ? Norm::Drop : Norm::Contradiction;
  }
  int64_t g = 0;
  for (const auto& [v, a] : k.coeffs) g = gcd(g, a);
  if (g > 1) {
    if (k.eq && k.c % g != 0) return Norm::Contradiction;
    for (auto& [v, a] : k.coeffs) a /= g;
    k.c = k.eq ? k.c / g : floorDiv(k.c, g);
  }
  return Norm::Keep;
}

class Omega {
 public:
  Omega(size_t budget, size_t max_constraints) : budget_(budget), max_(max_constraints) {}

  Sat solve(std::vector<Constraint> cs) {
    if (budget_ == 0 || cs.size() > max_) return Sat::Unknown;
    --budget_;
    std::vector<Constraint> eqs, geqs;
    for (Constraint& k : cs) {
      switch (normalize(k)) {
        case Norm::Contradiction: return Sat::Unsat;
        case Norm::Drop: break;
        case Norm::Keep: (k.eq ? eqs : geqs).push_back(std::move(k)); break;
      }
    }
    if (!eqs.empty()) return equality(std::move(eqs), std::move(geqs));
    return inequalities(std::move(geqs));
  }

 private:
  // Eliminate one equality: substitute exactly when some coefficient is +-1,
  // else shrink the coefficients by a unimodular change of variables
  // (x_k = t - sum q_i x_i - q_c), which keeps the integer solutions in
  // one-to-one correspondence and strictly lowers the smallest coefficient.
  Sat equality(std::vector<Constraint> eqs, std::vector<Constraint> geqs) {
    size_t best = 0;
    std::string var;
    int64_t smallest = 0;
    for (size_t i = 0; i < eqs.size(); ++i) {
      for (const auto& [v, a] : eqs[i].coeffs) {
        if (var.empty() || absval(a) < smallest) {
          best = i;
          var = v;
          smallest = absval(a);
        }
      }
    }
    Constraint e = eqs[best];
    eqs.erase(eqs.begin() + static_cast<std::ptrdiff_t>(best));
    std::vector<Constraint> rest = std::move(eqs);
    rest.insert(rest.end(), geqs.begin(), geqs.end());
    const int64_t ak = e.coeffs.at(var);

    Coeffs expr;
    int64_t ec = 0;
    if (smallest == 1) {  // x = -ak * (sum_{i != k} a_i x_i + c), since 1/ak = ak
      for (const auto& [v, a] : e.coeffs) {
        if (v != var) expr[v] = fit(-static_cast<__int128>(ak) * a);
      }
      ec = fit(-static_cast<__int128>(ak) * e.c);
    } else {
      expr["#t" + std::to_string(fresh_++)] = 1;
      for (const auto& [v, a] : e.coeffs) {
        if (v == var) continue;
        if (const int64_t q = nearestDiv(a, ak)) expr[v] = -q;
      }
      ec = -nearestDiv(e.c, ak);
      rest.push_back(e);  // the equality itself, now with smaller coefficients
    }
    for (Constraint& k : rest) substitute(k, var, expr, ec);
    return solve(std::move(rest));
  }

  Sat inequalities(std::vector<Constraint> geqs) {
    // The tightest constant per coefficient vector; opposite pairs either
    // contradict or pin an equality.
    std::map<Coeffs, int64_t> tight;
    for (const Constraint& k : geqs) {
      const auto it = tight.find(k.coeffs);
      if (it == tight.end() || k.c < it->second) tight[k.coeffs] = k.c;
    }
    for (const auto& [co, c] : tight) {
      Coeffs neg;
      for (const auto& [v, a] : co) neg[v] = -a;
      const auto it = tight.find(neg);
      if (it == tight.end()) continue;
      const __int128 sum = static_cast<__int128>(c) + it->second;
      if (sum < 0) return Sat::Unsat;
      if (sum == 0) {
        std::vector<Constraint> next{{co, c, true}};
        for (const auto& [co2, c2] : tight) {
          if (co2 != co && co2 != neg) next.push_back({co2, c2, false});
        }
        return solve(std::move(next));
      }
    }
    if (tight.empty()) return Sat::Sat;

    // A variable bounded on one side only can always be satisfied: drop it.
    std::map<std::string, std::pair<int, int>> sides;  // lower, upper counts
    for (const auto& [co, c] : tight) {
      for (const auto& [v, a] : co) (a > 0 ? sides[v].first : sides[v].second)++;
    }
    std::set<std::string> loose;
    for (const auto& [v, lu] : sides) {
      if (lu.first == 0 || lu.second == 0) loose.insert(v);
    }
    if (!loose.empty()) {
      std::vector<Constraint> kept;
      for (const auto& [co, c] : tight) {
        bool touches = false;
        for (const auto& [v, a] : co) touches = touches || loose.count(v);
        if (!touches) kept.push_back({co, c, false});
      }
      return solve(std::move(kept));
    }

    // Eliminate the variable with an exact projection if there is one (all
    // lower or all upper coefficients are 1), else the cheapest.
    std::string x;
    bool x_exact = false;
    long x_cost = 0;
    for (const auto& [v, lu] : sides) {
      bool lower_unit = true, upper_unit = true;
      for (const auto& [co, c] : tight) {
        const auto it = co.find(v);
        if (it == co.end()) continue;
        if (it->second > 0 && it->second != 1) lower_unit = false;
        if (it->second < 0 && it->second != -1) upper_unit = false;
      }
      const bool exact = lower_unit || upper_unit;
      const long cost = static_cast<long>(lu.first) * lu.second;
      if (x.empty() || (exact && !x_exact) || (exact == x_exact && cost < x_cost)) {
        x = v;
        x_exact = exact;
        x_cost = cost;
      }
    }

    std::vector<Constraint> others, lowers, uppers;
    for (const auto& [co, c] : tight) {
      const auto it = co.find(x);
      Constraint k{co, c, false};
      if (it == co.end()) others.push_back(std::move(k));
      else (it->second > 0 ? lowers : uppers).push_back(std::move(k));
    }
    if (others.size() + lowers.size() * uppers.size() > max_) return Sat::Unknown;
    // b*x + L >= 0 (b > 0) and -a*x + U >= 0 (a > 0) project to b*U + a*L >= 0
    // (real shadow), or >= (a-1)(b-1) for an integer x to fit (dark shadow).
    auto shadow = [&](bool dark) {
      std::vector<Constraint> out = others;
      for (const Constraint& lo : lowers) {
        const int64_t b = lo.coeffs.at(x);
        for (const Constraint& up : uppers) {
          const int64_t a = -up.coeffs.at(x);
          Constraint k;
          for (const auto& [v, w] : up.coeffs) {
            if (v != x) k.coeffs[v] = fit(static_cast<__int128>(k.coeffs[v]) + static_cast<__int128>(b) * w);
          }
          for (const auto& [v, w] : lo.coeffs) {
            if (v != x) k.coeffs[v] = fit(static_cast<__int128>(k.coeffs[v]) + static_cast<__int128>(a) * w);
          }
          __int128 c = static_cast<__int128>(b) * up.c + static_cast<__int128>(a) * lo.c;
          if (dark) c -= static_cast<__int128>(a - 1) * (b - 1);
          k.c = fit(c);
          out.push_back(std::move(k));
        }
      }
      return out;
    };
    if (x_exact) return solve(shadow(false));

    const Sat real = solve(shadow(false));
    if (real == Sat::Unsat) return Sat::Unsat;
    bool unknown = real == Sat::Unknown;
    const Sat dark = solve(shadow(true));
    if (dark == Sat::Sat) return Sat::Sat;
    unknown = unknown || dark == Sat::Unknown;

    // Splinters: an integer solution outside the dark shadow lies close to some
    // lower bound, b*x = -L + j for 0 <= j <= (m*b - m - b) / m.
    int64_t m = 0;
    for (const Constraint& up : uppers) m = std::max(m, -up.coeffs.at(x));
    std::vector<Constraint> all = others;
    all.insert(all.end(), lowers.begin(), lowers.end());
    all.insert(all.end(), uppers.begin(), uppers.end());
    for (const Constraint& lo : lowers) {
      const int64_t b = lo.coeffs.at(x);
      const int64_t last = floorDiv(fit(static_cast<__int128>(m) * b - m - b), m);
      for (int64_t j = 0; j <= last; ++j) {
        std::vector<Constraint> cs = all;
        cs.push_back({lo.coeffs, fit(static_cast<__int128>(lo.c) - j), true});
        const Sat s = solve(std::move(cs));
        if (s == Sat::Sat) return Sat::Sat;
        unknown = unknown || s == Sat::Unknown;
      }
    }
    return unknown ? Sat::Unknown : Sat::Unsat;
  }

  size_t budget_;
  size_t max_;
  int fresh_ = 0;
};

}  // namespace

Sat satisfiable(std::vector<Constraint> constraints, size_t budget, size_t max_constraints) {
  try {
    return Omega(budget, max_constraints).solve(std::move(constraints));
  } catch (const Overflow&) {
    return Sat::Unknown;
  }
}

}  // namespace brrr::symbolic::presburger
