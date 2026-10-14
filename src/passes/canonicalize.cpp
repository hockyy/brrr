// canonicalize: rewrite every integer expression — expression operands, list
// elements, and the dimensions of op result types — to its simplest form that
// the solver proves equal:
//   1. a constant, when the facts in scope pin it to one;
//   2. otherwise Z3's algebraic simplification, when it stays in the expression
//      language and is smaller.
// Argument types are left as written: they are the function's signature.
#include "passes/pass.h"
#include "sema/sema.h"
#include "symbolic/expr_io.h"
#include "symbolic/z3_backend.h"

namespace {

using namespace brrr;
using symbolic::Expr;
using symbolic::ExprTerm;

size_t size(const Expr& e) {
  size_t n = 1;
  for (const Expr& a : e->args) n += size(a);
  return n;
}

Expr simplest(const Expr& e, const symbolic::Solver& s) {
  if (e->kind == ExprTerm::Kind::Const) return nullptr;
  if (e->kind == ExprTerm::Kind::Var && e->name == "?") return nullptr;
  if (const auto c = s.constantValue(e)) return ExprTerm::constant(*c);
  const Expr z = symbolic::z3::simplify(e);
  if (!z || size(z) >= size(e)) return nullptr;
  return s.isEqual(e, z) == symbolic::Tri::Yes ? z : nullptr;
}

bool canonicalize(Module& m, const passes::Context& ctx, std::string*) {
  sema::Sema sema(m, ctx.reg);  // every rewrite is an equality, so its view stays valid
  auto show = [&](const Expr& e) { return sema.show(e); };
  for (Node& n : m.nodes) {
    if (schema::isStructural(n.kind)) continue;
    const symbolic::Solver& s = sema.solverFor(n.scope);
    std::string notes;
    auto note = [&](const Expr& from, const Expr& to) {
      notes += (notes.empty() ? "" : "; ") + show(from) + " -> " + show(to);
    };
    for (Operand& o : n.operands) {
      if (o.kind == Operand::Kind::Value) continue;
      for (Expr& e : o.exprs) {
        if (const Expr better = simplest(e, s)) {
          note(e, better);
          e = better;
        }
      }
      ValueRef v;  // an expression that became a single value is a value operand
      if (o.kind == Operand::Kind::Expr && o.exprs[0]->kind == ExprTerm::Kind::Var &&
          parseValueAtom(o.exprs[0]->name, &v)) {
        o = Operand::ofValue(v);
      }
    }
    for (Type& t : n.types) {
      if (!t || !t->shaped) continue;
      auto t2 = std::make_shared<TypeTerm>(*t);
      bool changed = false;
      for (Expr& d : t2->dims) {
        if (const Expr better = simplest(d, s)) {
          note(d, better);
          d = better;
          changed = true;
        }
      }
      if (changed) t = t2;
    }
    if (!notes.empty()) m.log("canonicalize", n.id, notes);
  }
  return true;
}

}  // namespace

BRRR_PASS(canonicalize, "canonicalize",
              "simplify every integer expression to its simplest proven-equal form (constants "
              "from facts; Z3 simplification)",
              canonicalize);
