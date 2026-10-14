// fold-constants: replace integer values the solver pins to a constant with
// `const`, and fold expression operands to constants.
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;

bool foldConstants(Module& m, const passes::Context& ctx, std::string*) {
  sema::Sema sema(m, ctx.reg);
  struct Fold {
    ValueRef v;
    int64_t c;
    NodeId scope;
    Type type;
  };
  std::vector<Fold> values;
  for (const Node& n : m.nodes) {
    if (schema::isStructural(n.kind) || n.kind == "const" || n.is_scope) continue;
    if (n.numResults() != 1 || !passes::isPure(ctx, n)) continue;
    const ValueRef v{n.id, 0};
    const symbolic::Expr e = sema.meaning(v);
    if (e->kind == symbolic::ExprTerm::Kind::Var && e->name == valueAtom(v)) continue;
    if (auto c = sema.solverFor(n.scope).constantValue(e)) {
      values.push_back({v, *c, n.scope, sema.typeOf(v)});
    }
  }
  for (Node& n : m.nodes) {
    if (schema::isStructural(n.kind)) continue;
    const symbolic::Solver& s = sema.solverFor(n.scope);
    bool changed = false;
    for (Operand& o : n.operands) {
      if (o.kind == Operand::Kind::Value) continue;
      for (auto& e : o.exprs) {
        if (e->kind == symbolic::ExprTerm::Kind::Const) continue;
        if (auto c = s.constantValue(e)) {
          e = symbolic::ExprTerm::constant(*c);
          changed = true;
        }
      }
    }
    if (changed) m.log("fold", n.id, "expression operands folded to constants");
  }
  for (const Fold& f : values) {
    Node c;
    c.kind = "const";
    c.scope = f.scope;
    c.names.push_back("");
    c.types.push_back(f.type);
    c.attrs["value"] = AttrValue::fromInt(f.c);
    const NodeId cid = m.insertAfter(f.v.id, std::move(c)).id;
    m.log("insert", cid, "const " + std::to_string(f.c) + " for " + valueAtom(f.v));
    m.replaceAllUses(f.v, {cid, 0});
  }
  return true;
}

}  // namespace

BRRR_PASS(fold_constants, "fold-constants",
              "replace integer values the solver pins to a constant with `const`; fold "
              "expression operands",
              foldConstants);
