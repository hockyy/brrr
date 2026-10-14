#include <algorithm>

#include "passes/pass.h"

namespace brrr::passes {
namespace {

// Function-local, so registration from any file's static initializer is safe.
std::vector<PassInfo>& passes() {
  static std::vector<PassInfo> all;
  return all;
}

}  // namespace

Registrar::Registrar(PassInfo p) { passes().push_back(std::move(p)); }

std::vector<PassInfo> registry() {
  std::vector<PassInfo> out = passes();
  std::sort(out.begin(), out.end(),
            [](const PassInfo& a, const PassInfo& b) { return a.name < b.name; });
  return out;
}

const PassInfo* find(const std::string& name) {
  for (const PassInfo& p : passes()) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

bool isPure(const Context& ctx, const Node& n) {
  const schema::Kind* k = ctx.reg.find(n.kind);
  return k && k->hasTrait("pure") && !n.is_scope;
}

}  // namespace brrr::passes
