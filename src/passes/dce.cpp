// dce: remove pure ops whose results are never used, to a fixed point.
#include "passes/pass.h"

namespace {

using namespace brrr;

bool dce(Module& m, const passes::Context& ctx, std::string*) {
  for (bool again = true; again;) {
    again = false;
    for (const Node& n : m.nodes) {
      if (!passes::isPure(ctx, n) || n.numResults() == 0) continue;
      bool used = false;
      for (uint32_t p = 0; p < n.numResults() && !used; ++p) used = m.useCount({n.id, p}) > 0;
      if (!used) {
        m.erase(n.id);
        again = true;
        break;
      }
    }
  }
  return true;
}

}  // namespace

BRRR_PASS(dce, "dce", "remove pure ops whose results are unused", dce);
