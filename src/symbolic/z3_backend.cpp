#include "symbolic/z3_backend.h"

#ifdef BRRR_HAS_Z3

#include <z3.h>

#include <vector>

namespace brrr::symbolic::z3 {
namespace {

constexpr unsigned kTimeoutMs = 200;  // per check; a timeout is an honest Unknown

bool g_error = false;
void onError(Z3_context, Z3_error_code) { g_error = true; }

// One context for the process: ASTs are managed by it (non-rc context).
Z3_context ctx() {
  static Z3_context c = [] {
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

}  // namespace

bool available() { return true; }

std::string version() {
  unsigned major, minor, build, rev;
  Z3_get_version(&major, &minor, &build, &rev);
  return "z3 " + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build);
}

struct Session::Impl {
  Z3_solver solver;
  Impl() {
    Z3_context c = ctx();
    solver = Z3_mk_solver(c);
    Z3_solver_inc_ref(c, solver);
    Z3_params p = Z3_mk_params(c);
    Z3_params_inc_ref(c, p);
    Z3_params_set_uint(c, p, Z3_mk_string_symbol(c, "timeout"), kTimeoutMs);
    Z3_solver_set_params(c, solver, p);
    Z3_params_dec_ref(c, p);
  }
  ~Impl() { Z3_solver_dec_ref(ctx(), solver); }

  void add(Z3_ast f) { Z3_solver_assert(ctx(), solver, f); }

  // Satisfiable with `extra` added? FALSE / TRUE / UNDEF (timeout or error).
  Z3_lbool checkWith(Z3_ast extra) {
    Z3_context c = ctx();
    g_error = false;
    Z3_solver_push(c, solver);
    Z3_solver_assert(c, solver, extra);
    const Z3_lbool r = Z3_solver_check(c, solver);
    Z3_solver_pop(c, solver, 1);
    return g_error ? Z3_L_UNDEF : r;
  }

  Tri valid(Z3_ast q) {
    Z3_context c = ctx();
    if (checkWith(Z3_mk_not(c, q)) == Z3_L_FALSE) return Tri::Yes;
    if (checkWith(q) == Z3_L_FALSE) return Tri::No;
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
  g_error = false;
  if (Z3_solver_check(c, impl_->solver) != Z3_L_TRUE || g_error) return std::nullopt;
  Z3_model m = Z3_solver_get_model(c, impl_->solver);
  Z3_model_inc_ref(c, m);
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
