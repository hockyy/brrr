#include "symbolic/z3_backend.h"

#ifdef BRRR_HAS_Z3

#include <z3.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace brrr::symbolic::z3 {
namespace {

// Calibrated over the test suite and the examples (docs/scratch/solver.md): the
// most any check there consumes is about 570k (a degree-4 product in
// 06_shapes.br); the default is about 10x that.
constexpr uint64_t kDefaultRlimit = 6000000;
// Far above what the rlimit allows; hitting it means the limit is miscalibrated.
constexpr unsigned kSafetyNetMs = 10000;

uint64_t g_rlimit = [] {
  const char* v = std::getenv("BRRR_SOLVER_RLIMIT");
  return v && *v ? std::strtoull(v, nullptr, 10) : kDefaultRlimit;
}();
Stats g_stats;

bool g_error = false;
void onError(Z3_context, Z3_error_code) { g_error = true; }

// One context for the process: ASTs are managed by it (non-rc context).
Z3_context ctx() {
  static Z3_context c = [] {
    Z3_global_param_set("smt.random_seed", "0");
    Z3_global_param_set("sat.random_seed", "0");
    Z3_config cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "model", "true");
    Z3_context made = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    Z3_set_error_handler(made, onError);
    return made;
  }();
  return c;
}

Z3_sort intSort() { return Z3_mk_int_sort(ctx()); }

Z3_ast toZ3(const Expr& e) {
  Z3_context c = ctx();
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return Z3_mk_int64(c, e->c, intSort());
    case ExprTerm::Kind::Var:
      return Z3_mk_const(c, Z3_mk_string_symbol(c, e->name.c_str()), intSort());
    case ExprTerm::Kind::Add:
    case ExprTerm::Kind::Mul: {
      std::vector<Z3_ast> args;
      for (const Expr& a : e->args) args.push_back(toZ3(a));
      const unsigned n = static_cast<unsigned>(args.size());
      return e->kind == ExprTerm::Kind::Add ? Z3_mk_add(c, n, args.data())
                                            : Z3_mk_mul(c, n, args.data());
    }
    case ExprTerm::Kind::FloorDiv:
      return Z3_mk_div(c, toZ3(e->args[0]), toZ3(e->args[1]));
    case ExprTerm::Kind::CeilDiv:
      return Z3_mk_unary_minus(
          c, Z3_mk_div(c, Z3_mk_unary_minus(c, toZ3(e->args[0])), toZ3(e->args[1])));
    case ExprTerm::Kind::Mod:
      return Z3_mk_mod(c, toZ3(e->args[0]), toZ3(e->args[1]));
  }
  return Z3_mk_int64(c, 0, intSort());
}

Expr fromZ3(Z3_ast a) {
  Z3_context c = ctx();
  if (Z3_get_ast_kind(c, a) == Z3_NUMERAL_AST) {
    int64_t v = 0;
    return Z3_get_numeral_int64(c, a, &v) ? ExprTerm::constant(v) : nullptr;
  }
  if (Z3_get_ast_kind(c, a) != Z3_APP_AST) return nullptr;
  Z3_app app = Z3_to_app(c, a);
  Z3_func_decl d = Z3_get_app_decl(c, app);
  const unsigned n = Z3_get_app_num_args(c, app);
  std::vector<Expr> args;
  for (unsigned i = 0; i < n; ++i) {
    Expr x = fromZ3(Z3_get_app_arg(c, app, i));
    if (!x) return nullptr;
    args.push_back(x);
  }
  auto positiveConstant = [&](const Expr& x) {
    return x->kind == ExprTerm::Kind::Const && x->c > 0;
  };
  switch (Z3_get_decl_kind(c, d)) {
    case Z3_OP_UNINTERPRETED:
      if (n != 0) return nullptr;
      return ExprTerm::var(Z3_get_symbol_string(c, Z3_get_decl_name(c, d)));
    case Z3_OP_ADD:
      return ExprTerm::add(std::move(args));
    case Z3_OP_MUL:
      return ExprTerm::mul(std::move(args));
    case Z3_OP_SUB: {
      if (args.empty()) return nullptr;
      std::vector<Expr> terms{args[0]};
      for (size_t i = 1; i < args.size(); ++i) {
        terms.push_back(ExprTerm::mul({ExprTerm::constant(-1), args[i]}));
      }
      return ExprTerm::add(std::move(terms));
    }
    case Z3_OP_UMINUS:
      return n == 1 ? ExprTerm::mul({ExprTerm::constant(-1), args[0]}) : nullptr;
    case Z3_OP_IDIV:
      return n == 2 && positiveConstant(args[1])
                 ? ExprTerm::div(ExprTerm::Kind::FloorDiv, args[0], args[1]->c)
                 : nullptr;
    case Z3_OP_MOD:
      return n == 2 && positiveConstant(args[1])
                 ? ExprTerm::div(ExprTerm::Kind::Mod, args[0], args[1]->c)
                 : nullptr;
    default:
      return nullptr;  // ite, rem, ... : outside the expression language
  }
}

// What one check consumed, from the solver's statistics.
uint64_t rlimitUsed(Z3_solver s) {
  Z3_context c = ctx();
  Z3_stats st = Z3_solver_get_statistics(c, s);
  Z3_stats_inc_ref(c, st);
  uint64_t used = 0;
  for (unsigned i = 0; i < Z3_stats_size(c, st); ++i) {
    if (std::string(Z3_stats_get_key(c, st, i)) != "rlimit count") continue;
    used = Z3_stats_is_uint(c, st, i)
               ? Z3_stats_get_uint_value(c, st, i)
               : static_cast<uint64_t>(Z3_stats_get_double_value(c, st, i));
  }
  Z3_stats_dec_ref(c, st);
  return used;
}

}  // namespace

bool available() { return true; }

std::string version() {
  unsigned major, minor, build, rev;
  Z3_get_version(&major, &minor, &build, &rev);
  return "z3 " + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build);
}

uint64_t rlimit() { return g_rlimit; }
void setRlimit(uint64_t limit) { g_rlimit = limit; }
const Stats& stats() { return g_stats; }

struct Session::Impl {
  std::vector<Z3_ast> facts;  // in the order they were asserted

  void add(Z3_ast f) { facts.push_back(f); }

  // A fresh solver with the facts (and `extra`), checked once. FALSE / TRUE /
  // UNDEF (limit, safety net, incompleteness, or an error). With `model`, a
  // satisfying model is returned (the caller releases it).
  Z3_lbool check(Z3_ast extra, Z3_model* model = nullptr) {
    Z3_context c = ctx();
    g_error = false;
    Z3_solver s = Z3_mk_solver(c);
    Z3_solver_inc_ref(c, s);
    Z3_params p = Z3_mk_params(c);
    Z3_params_inc_ref(c, p);
    if (g_rlimit) {
      const unsigned limit = g_rlimit > UINT_MAX ? UINT_MAX : static_cast<unsigned>(g_rlimit);
      Z3_params_set_uint(c, p, Z3_mk_string_symbol(c, "rlimit"), limit);
    }
    Z3_params_set_uint(c, p, Z3_mk_string_symbol(c, "timeout"), kSafetyNetMs);
    Z3_solver_set_params(c, s, p);
    Z3_params_dec_ref(c, p);
    for (Z3_ast f : facts) Z3_solver_assert(c, s, f);
    if (extra) Z3_solver_assert(c, s, extra);
    // The counter is the context's running total; a check's cost is the difference.
    const uint64_t before = rlimitUsed(s);
    const Z3_lbool r = Z3_solver_check(c, s);
    ++g_stats.checks;
    const uint64_t total = rlimitUsed(s);
    const uint64_t used = total >= before ? total - before : total;
    g_stats.last_rlimit_used = used;
    if (used > g_stats.max_rlimit_used) g_stats.max_rlimit_used = used;
    static const bool trace = [] {
      const char* v = std::getenv("BRRR_SOLVER_STATS");
      return v && std::string(v) == "2";
    }();
    if (trace) {
      std::string q = extra ? Z3_ast_to_string(c, extra) : "(model)";
      for (char& ch : q) ch = ch == '\n' ? ' ' : ch;
      std::fprintf(stderr, "solver check: rlimit %llu  %s  %s\n",
                   static_cast<unsigned long long>(used),
                   r == Z3_L_TRUE ? "sat" : r == Z3_L_FALSE ? "unsat" : "unknown", q.c_str());
    }
    if (r == Z3_L_UNDEF) {
      const std::string why = Z3_solver_get_reason_unknown(c, s);
      if (why.find("resource") != std::string::npos) {
        ++g_stats.resource_unknowns;
      } else if (why.find("timeout") != std::string::npos ||
                 why.find("canceled") != std::string::npos) {
        if (g_stats.safety_net_hits++ == 0) {
          std::fprintf(stderr,
                       "solver: a check hit the %u ms wall-clock safety net, so its Unknown is "
                       "not reproducible; lower BRRR_SOLVER_RLIMIT\n",
                       kSafetyNetMs);
        }
      }
    }
    if (model && r == Z3_L_TRUE && !g_error) {
      *model = Z3_solver_get_model(c, s);
      Z3_model_inc_ref(c, *model);
    }
    Z3_solver_dec_ref(c, s);
    return g_error ? Z3_L_UNDEF : r;
  }

  Tri valid(Z3_ast q) {
    Z3_context c = ctx();
    if (check(Z3_mk_not(c, q)) == Z3_L_FALSE) return Tri::Yes;
    if (check(q) == Z3_L_FALSE) return Tri::No;
    return Tri::Unknown;
  }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

void Session::assertEqual(const Expr& a, const Expr& b) {
  impl_->add(Z3_mk_eq(ctx(), toZ3(a), toZ3(b)));
}
void Session::assertLe(const Expr& a, const Expr& b) {
  impl_->add(Z3_mk_le(ctx(), toZ3(a), toZ3(b)));
}
void Session::assertRange(const std::string& atom, int64_t lo, int64_t hi) {
  const Expr v = ExprTerm::var(atom);
  assertLe(ExprTerm::constant(lo), v);
  assertLe(v, ExprTerm::constant(hi));
}
void Session::assertMod(const std::string& atom, int64_t modulus, int64_t residue) {
  impl_->add(Z3_mk_eq(ctx(), toZ3(ExprTerm::div(ExprTerm::Kind::Mod, ExprTerm::var(atom), modulus)),
                      toZ3(ExprTerm::constant(residue))));
}

Tri Session::isEqual(const Expr& a, const Expr& b) {
  return impl_->valid(Z3_mk_eq(ctx(), toZ3(a), toZ3(b)));
}
Tri Session::lessOrEqual(const Expr& a, const Expr& b) {
  return impl_->valid(Z3_mk_le(ctx(), toZ3(a), toZ3(b)));
}
Tri Session::isLess(const Expr& a, const Expr& b) {
  return impl_->valid(Z3_mk_lt(ctx(), toZ3(a), toZ3(b)));
}
Tri Session::aligned(const Expr& e, int64_t modulus) {
  if (modulus <= 0) return Tri::Unknown;
  return isEqual(ExprTerm::div(ExprTerm::Kind::Mod, e, modulus), ExprTerm::constant(0));
}

// A model gives a candidate value; it is the constant iff no other value fits.
std::optional<int64_t> Session::constantValue(const Expr& e) {
  Z3_context c = ctx();
  Z3_model m = nullptr;
  if (impl_->check(nullptr, &m) != Z3_L_TRUE || !m) return std::nullopt;
  Z3_ast v = nullptr;
  int64_t value = 0;
  const bool ok = Z3_model_eval(c, m, toZ3(e), true, &v) && v &&
                  Z3_get_numeral_int64(c, v, &value);
  Z3_model_dec_ref(c, m);
  if (!ok) return std::nullopt;
  if (isEqual(e, ExprTerm::constant(value)) != Tri::Yes) return std::nullopt;
  return value;
}

Expr simplify(const Expr& e) {
  g_error = false;
  Z3_ast s = Z3_simplify(ctx(), toZ3(e));
  return g_error ? nullptr : fromZ3(s);
}

}  // namespace brrr::symbolic::z3

#else  // no Z3: every question stays open

namespace brrr::symbolic::z3 {

bool available() { return false; }
std::string version() { return "none"; }
uint64_t rlimit() { return 0; }
void setRlimit(uint64_t) {}
const Stats& stats() {
  static const Stats none;
  return none;
}

struct Session::Impl {};
Session::Session() = default;
Session::~Session() = default;
void Session::assertEqual(const Expr&, const Expr&) {}
void Session::assertLe(const Expr&, const Expr&) {}
void Session::assertRange(const std::string&, int64_t, int64_t) {}
void Session::assertMod(const std::string&, int64_t, int64_t) {}
Tri Session::isEqual(const Expr&, const Expr&) { return Tri::Unknown; }
Tri Session::lessOrEqual(const Expr&, const Expr&) { return Tri::Unknown; }
Tri Session::isLess(const Expr&, const Expr&) { return Tri::Unknown; }
Tri Session::aligned(const Expr&, int64_t) { return Tri::Unknown; }
std::optional<int64_t> Session::constantValue(const Expr&) { return std::nullopt; }
Expr simplify(const Expr&) { return nullptr; }

}  // namespace brrr::symbolic::z3

#endif
