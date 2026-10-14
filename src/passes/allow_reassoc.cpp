// allow-reassoc: grant float loop reductions permission to be reordered, so the
// compiler can vectorize them and keep several accumulators.
//
// A reduction is a float `add` that reads a block argument and whose result is
// passed back to that same block argument by a branch: the accumulator of a loop,
//
//   ^k_loop(%k: index, %acc: f32):
//     %sum = add %acc, %p : f32                    <- gets reassoc=true
//     cond_br %more, ^k_loop(%k2, %sum), ^done(%sum)
//
// With `contract=true`, the add and the float `mul` feeding it also get
// contract=true, so `acc + x * w` can become one FMA. `reassoc=false` grants
// contract alone: right for loops that vectorize across independent
// accumulators (tile-linear form=outer), where reordering the sum would steer
// the vectorizer the wrong way. Both permissions change
// rounding in the last bits: that is the agent's call, recorded per op in the
// journal, and checked against a reference (examples/build.py compares with torch).
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;

bool isFloat(const schema::Registry& reg, const Type& t) {
  const schema::Scalar* s = t && !t->shaped ? reg.findScalar(t->name) : nullptr;
  return s && s->domain == "float";
}

bool allowReassoc(Module& m, const passes::Context& ctx, std::string*) {
  auto flag = [&](const char* key, bool fallback) {
    const auto it = ctx.options.find(key);
    return it == ctx.options.end() ? fallback : it->second == "true";
  };
  const bool reassoc = flag("reassoc", true);    // reorder the sum: vectorize along it
  const bool contract = flag("contract", false);  // fuse a * b + c into an FMA
  sema::Sema sema(m, ctx.reg);

  // Every value a branch passes to block argument i of block b.
  std::map<ValueRef, std::vector<ValueRef>> passed_to;  // block arg -> values passed
  for (const Node& n : m.nodes) {
    for (const Successor& s : n.successors) {
      std::vector<NodeId> args;
      for (NodeId c : m.childrenOf(s.block)) {
        if (m.find(c)->kind == "arg") args.push_back(c);
      }
      for (size_t i = 0; i < s.args.size() && i < args.size(); ++i) {
        passed_to[{args[i], 0}].push_back(s.args[i]);
      }
    }
  }

  std::vector<std::pair<NodeId, std::string>> grants;
  for (const Node& n : m.nodes) {
    if (n.kind != "add" || n.operands.size() != 2 || !isFloat(ctx.reg, sema.typeOf({n.id, 0}))) {
      continue;
    }
    for (size_t side = 0; side < 2; ++side) {
      const Operand& acc = n.operands[side];
      if (acc.kind != Operand::Kind::Value) continue;
      const Node* def = m.find(acc.value.id);
      if (!def || def->kind != "arg" || m.find(def->scope)->kind != "block") continue;
      const auto it = passed_to.find(acc.value);
      if (it == passed_to.end()) continue;
      bool carried = false;  // the sum flows back into the accumulator
      for (const ValueRef& v : it->second) carried = carried || v == ValueRef{n.id, 0};
      if (!carried) continue;
      grants.push_back({n.id, "reduction into " + valueAtom(acc.value)});
      const Operand& term = n.operands[1 - side];
      if (contract && term.kind == Operand::Kind::Value) {
        const Node* mul = m.find(term.value.id);
        if (mul && mul->kind == "mul" && isFloat(ctx.reg, sema.typeOf(term.value))) {
          grants.push_back({mul->id, "contract: feeds the reduction"});
        }
      }
      break;
    }
  }

  for (const auto& [id, why] : grants) {
    Node* n = m.find(id);
    std::string flags;
    if (n->kind == "add" && reassoc) {
      n->attrs["reassoc"] = AttrValue::fromBool(true);
      flags = "reassoc";
    }
    if (contract) {
      n->attrs["contract"] = AttrValue::fromBool(true);
      flags += flags.empty() ? "contract" : " contract";
    }
    m.log("allow-reassoc", id, flags + " (" + why + ")");
  }
  return true;
}

}  // namespace

BRRR_PASS(allow_reassoc, "allow-reassoc",
          "let float loop reductions be reordered (reassoc) so they vectorize; "
          "contract=true also allows FMA",
          allowReassoc);
