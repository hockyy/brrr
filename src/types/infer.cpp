#include "types/infer.h"

namespace bridgeir::types {
namespace {

TypeRef subst(const TypeRef& t, const std::vector<TypeRef>& map) {
  if (t->kind == TypeTerm::Kind::Var) {
    return t->var < map.size() ? map[t->var] : t;
  }
  std::vector<TypeRef> args;
  args.reserve(t->args.size());
  for (const auto& a : t->args) args.push_back(subst(a, map));
  return TypeTerm::makeName(t->name, std::move(args));
}

}  // namespace

InferResult infer(const std::vector<InferNode>& nodes,
                  const std::vector<Signature>& signatures) {
  InferResult res;
  Unifier u;
  std::vector<TypeRef> types(nodes.size());

  for (size_t ni = 0; ni < nodes.size(); ++ni) {
    const InferNode& n = nodes[ni];
    const Signature* sig = nullptr;
    for (const auto& s : signatures) {
      if (s.kind == n.kind) {
        sig = &s;
        break;
      }
    }
    if (!sig) {  // unknown kinds are permissive: a fresh variable
      types[ni] = n.given ? n.given : u.fresh();
      continue;
    }

    std::vector<TypeRef> inst(sig->type_params);
    for (auto& v : inst) v = u.fresh();
    auto instTerm = [&](const TypeRef& t) { return subst(t, inst); };

    if (sig->operand_types.size() != n.operands.size()) {
      res.ok = false;
      res.error = "n" + std::to_string(n.id) + " (" + n.kind + "): expects " +
                  std::to_string(sig->operand_types.size()) + " operands, got " +
                  std::to_string(n.operands.size());
      return res;
    }
    for (size_t i = 0; i < n.operands.size(); ++i) {
      std::string err;
      if (!u.unify(types[n.operands[i]], instTerm(sig->operand_types[i]), &err)) {
        res.ok = false;
        res.error = "n" + std::to_string(n.id) + " (" + n.kind + ") operand " +
                    std::to_string(i) + ": " + err;
        return res;
      }
    }
    TypeRef result = instTerm(sig->result_type);
    if (n.given) {
      std::string err;
      if (!u.unify(n.given, result, &err)) {
        res.ok = false;
        res.error = "n" + std::to_string(n.id) + " (" + n.kind +
                    ") declared type: " + err;
        return res;
      }
    }
    types[ni] = result;
  }

  res.types.reserve(types.size());
  for (const auto& t : types) res.types.push_back(u.resolve(t));
  for (size_t i = 0; i < nodes.size(); ++i) {
    res.equations.push_back("n" + std::to_string(nodes[i].id) + ": " +
                            nodes[i].kind + " : " + u.pretty(types[i]));
  }
  return res;
}

}  // namespace bridgeir::types
