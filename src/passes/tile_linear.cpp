// tile-linear rows=R cols=C [form=inner|outer]: schedule every aten.linear for
// register blocking. form=outer reads transposed weights so a row's C outputs
// vectorize across columns (see lower-aten).
//
// A schedule, not a rewrite: it records the tile on the op (attrs rows, cols) and
// lower-aten realizes it. The tile must divide the layer's dims, and that is a
// solver question asked under the facts in scope:
//
//   rows=R   the token dim B:      aligned(B, R)    e.g. B = 8 * %t, R = 4: Yes
//   cols=C   the output dim N:     aligned(N, C)    e.g. N = 256,    C = 8: Yes
//
// Anything but Yes refuses the stage (no remainder loops yet), naming the
// question, so an agent knows which fact would make the tile legal.
#include "passes/pass.h"
#include "sema/sema.h"
#include "symbolic/expr_io.h"

namespace {

using namespace brrr;

int64_t option(const passes::Context& ctx, const char* key, int64_t fallback) {
  const auto it = ctx.options.find(key);
  return it == ctx.options.end() ? fallback : std::atoll(it->second.c_str());
}

bool tileLinear(Module& m, const passes::Context& ctx, std::string* err) {
  const int64_t R = option(ctx, "rows", 4), C = option(ctx, "cols", 4);
  const auto f = ctx.options.find("form");
  const std::string form = f == ctx.options.end() ? "inner" : f->second;
  if (form != "inner" && form != "outer") {
    *err = "form must be inner or outer, got '" + form + "'";
    return false;
  }
  if (R < 1 || C < 1) {
    *err = "rows and cols must be positive";
    return false;
  }
  sema::Sema sema(m, ctx.reg);
  std::vector<NodeId> linears;
  for (const Node& n : m.nodes) {
    if (n.kind == "aten.linear" && n.operands.size() == 3) linears.push_back(n.id);
  }
  for (NodeId id : linears) {
    const Node& n = *m.find(id);
    const Type xt = n.operands[0].kind == Operand::Kind::Value ? sema.typeOf(n.operands[0].value) : nullptr;
    const Type wt = n.operands[1].kind == Operand::Kind::Value ? sema.typeOf(n.operands[1].value) : nullptr;
    if (!xt || !wt || xt->dims.size() != 2 || wt->dims.size() != 2) {
      *err = "n" + std::to_string(id) + " (aten.linear): needs 2-D input and weight";
      return false;
    }
    const symbolic::Solver& s = sema.solverFor(n.scope);
    const std::pair<const char*, std::pair<symbolic::Expr, int64_t>> checks[] = {
        {"rows", {xt->dims[0], R}}, {"cols", {wt->dims[0], C}}};
    for (const auto& [name, dt] : checks) {
      const symbolic::Tri r = s.aligned(dt.first, dt.second);
      if (r != symbolic::Tri::Yes) {
        *err = "n" + std::to_string(id) + " (aten.linear): " + name + "=" + std::to_string(dt.second) +
               " needs " + sema.show(dt.first) + " mod " + std::to_string(dt.second) + " == 0, and the solver says " +
               symbolic::toString(r) + " (add a fact, or pick a divisor)";
        return false;
      }
    }
    Node* mn = m.find(id);
    mn->attrs["rows"] = AttrValue::fromInt(R);
    mn->attrs["cols"] = AttrValue::fromInt(C);
    mn->attrs["form"] = AttrValue::fromStr(form);
    m.log("tile-linear", id,
          "rows=" + std::to_string(R) + " cols=" + std::to_string(C) + " (" + sema.show(xt->dims[0]) +
              " mod " + std::to_string(R) + " == 0 and " + sema.show(wt->dims[0]) + " mod " +
              std::to_string(C) + " == 0: proven), form=" + form);
  }
  return true;
}

}  // namespace

BRRR_PASS(tile_linear, "tile-linear",
          "schedule aten.linear for register blocking (rows=R cols=C), after the solver proves the "
          "tile divides the dims; lower-aten realizes it",
          tileLinear);
