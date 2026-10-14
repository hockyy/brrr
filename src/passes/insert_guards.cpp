// insert-guards: turn every equation the solver could not prove into a runtime
// `guard` right before the op that needs it. Below the guard it is a fact.
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;

bool insertGuards(Module& m, const passes::Context& ctx, std::string*) {
  std::vector<sema::Sema::Unproven> todo;
  {
    sema::Sema sema(m, ctx.reg);
    sema.check();
    todo = sema.unproven();
  }
  auto operandOf = [](const symbolic::Expr& e) {
    ValueRef v;
    if (e->kind == symbolic::ExprTerm::Kind::Var && parseValueAtom(e->name, &v)) {
      return Operand::ofValue(v);
    }
    return Operand::ofExpr(e);
  };
  for (const auto& u : todo) {
    const Node* at = m.find(u.node);
    if (!at) continue;
    Node g;
    g.kind = "guard";
    g.scope = at->scope;
    g.operands.push_back(operandOf(u.a));
    g.operands.push_back(operandOf(u.b));
    g.attrs["rel"] = AttrValue::fromIdent(u.rel);
    g.attrs["what"] = AttrValue::fromStr(u.what);
    const NodeId gid = m.insertBefore(u.node, std::move(g)).id;
    m.log("insert", gid, "guard for n" + std::to_string(u.node) + ": " + u.what);
  }
  return true;
}

}  // namespace

BRRR_PASS(insert_guards, "insert-guards",
              "turn each equation the solver could not prove into a runtime guard (then it is "
              "a fact)",
              insertGuards);
