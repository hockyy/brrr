// fold-reshape: reshape(reshape(x)) -> reshape(x); a reshape to the type it
// already has (proved by the solver) -> x.
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;

bool foldReshape(Module& m, const passes::Context& ctx, std::string*) {
  for (bool again = true; again;) {
    again = false;
    sema::Sema sema(m, ctx.reg);
    for (const Node& n : m.nodes) {
      if (n.kind != "reshape" || n.operands.empty() ||
          n.operands[0].kind != Operand::Kind::Value) {
        continue;
      }
      const ValueRef src = n.operands[0].value;
      const Type from = sema.typeOf(src);
      const Type to = n.typeOf(0);
      if (from && to && from->shaped && to->shaped && from->dims.size() == to->dims.size() &&
          from->elem == to->elem) {
        const symbolic::Solver& s = sema.solverFor(n.scope);
        bool same = true;
        for (size_t d = 0; d < to->dims.size() && same; ++d) {
          same = s.isEqual(from->dims[d], to->dims[d]) == symbolic::Tri::Yes;
        }
        if (same) {
          const NodeId id = n.id;
          m.log("fold", id, "reshape to its own type is the identity");
          m.replaceAllUses({id, 0}, src);
          m.erase(id);
          again = true;
          break;
        }
      }
      const Node* inner = m.find(src.id);
      if (inner && inner->kind == "reshape" && !inner->operands.empty() &&
          m.useCount(src) == 1) {
        const Operand through = inner->operands[0];
        const NodeId id = n.id;
        const NodeId inner_id = inner->id;
        m.find(id)->operands[0] = through;
        m.log("fold", id, "reshape of reshape n" + std::to_string(inner_id) + " composed");
        m.erase(inner_id);
        again = true;
        break;
      }
    }
  }
  return true;
}

}  // namespace

BRRR_PASS(fold_reshape, "fold-reshape",
              "compose reshape chains; drop a reshape to the type it already has (proved by "
              "the solver)",
              foldReshape);
