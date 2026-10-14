// infer-shapes: write every inferred result type into the IR, so later stages
// (and readers) see it without re-running inference.
#include <set>

#include "passes/pass.h"
#include "sema/sema.h"
#include "symbolic/expr_io.h"

namespace {

using namespace brrr;

bool inferShapes(Module& m, const passes::Context& ctx, std::string*) {
  sema::Sema sema(m, ctx.reg);
  std::vector<std::pair<ValueRef, Type>> todo;
  for (const Node& n : m.nodes) {
    if (schema::isStructural(n.kind)) continue;
    for (uint32_t p = 0; p < n.numResults(); ++p) {
      const ValueRef v{n.id, p};
      if (n.typeOf(p) || !sema.isInferred(v)) continue;
      // A value's own fresh dimensions are written back as `?`; a type that
      // needs one inside a larger expression cannot be written, so it stays
      // inferred.
      auto t = std::make_shared<TypeTerm>(*sema.typeOf(v));
      bool writable = true;
      for (size_t k = 0; k < t->dims.size(); ++k) {
        std::set<std::string> atoms;
        symbolic::collectVars(t->dims[k], &atoms);
        for (const std::string& a : atoms) {
          std::string inner;
          size_t dim = 0;
          ValueRef owner;
          if (!symbolic::splitDimAtom(a, &inner, &dim) || !atomValue(a, &owner) ||
              !(owner == v)) {
            continue;
          }
          if (t->dims[k]->kind == symbolic::ExprTerm::Kind::Var && a == dimAtom(v, k)) {
            t->dims[k] = symbolic::ExprTerm::var("?");
          } else {
            writable = false;
          }
        }
      }
      if (writable) todo.emplace_back(v, t);
    }
  }
  for (const auto& [v, t] : todo) m.setType(v, t);
  return true;
}

}  // namespace

BRRR_PASS(infer_shapes, "infer-shapes",
              "write inferred result types (from shape rules) into the IR", inferShapes);
