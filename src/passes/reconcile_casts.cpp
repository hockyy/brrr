// reconcile-casts: fold unrealized casts that cancel — a cast to the type the
// value already has, and a round trip A -> B -> A. What remains is real work for
// a lowering.
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;

bool reconcileCasts(Module& m, const passes::Context& ctx, std::string*) {
  for (bool again = true; again;) {
    again = false;
    sema::Sema sema(m, ctx.reg);
    for (const Node& n : m.nodes) {
      const schema::Kind* k = ctx.reg.find(n.kind);
      if (!k || !k->hasTrait("unrealized") || n.operands.size() != 1 || n.numResults() != 1 ||
          n.operands[0].kind != Operand::Kind::Value) {
        continue;
      }
      const ValueRef in = n.operands[0].value;
      const Type to = n.typeOf(0);
      ValueRef replacement{kInvalidId, 0};
      if (to && typesEqualText(sema.typeOf(in), to)) replacement = in;  // A -> A
      const Node* inner = m.find(in.id);
      const schema::Kind* ik = inner ? ctx.reg.find(inner->kind) : nullptr;
      if (replacement.id == kInvalidId && ik && ik->hasTrait("unrealized") &&
          inner->operands.size() == 1 && inner->operands[0].kind == Operand::Kind::Value &&
          to && typesEqualText(sema.typeOf(inner->operands[0].value), to)) {
        replacement = inner->operands[0].value;  // A -> B -> A
      }
      if (replacement.id == kInvalidId) continue;
      const NodeId id = n.id;
      m.log("fold", id, "unrealized cast cancels");
      m.replaceAllUses({id, 0}, replacement);
      m.erase(id);
      again = true;
      break;
    }
  }
  return true;
}

}  // namespace

BRRR_PASS(reconcile_casts, "reconcile-casts",
              "fold unrealized casts that cancel (A -> A, A -> B -> A); what remains needs a "
              "lowering",
              reconcileCasts);
