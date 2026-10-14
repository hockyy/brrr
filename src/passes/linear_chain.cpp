#include "passes/linear_chain.h"

#include <cstdint>

namespace brrr::passes {

Dataflow::Dataflow(const Module& m, NodeId f) : func(f) {
  for (NodeId c : m.childrenOf(f)) {
    const Node* n = m.find(c);
    if (n->kind == "block") straight = false;
    position[c] = order.size();
    order.push_back(c);
    for (size_t i = 0; i < n->operands.size(); ++i) {
      const Operand& o = n->operands[i];
      if (o.kind == Operand::Kind::Value) {
        uses[o.value].push_back({c, i});
        continue;
      }
      std::vector<ValueRef> vs;
      o.uses(&vs);
      for (const ValueRef& v : vs) uses[v].push_back({c, SIZE_MAX});
    }
    for (const Successor& s : n->successors) {
      for (const ValueRef& v : s.args) uses[v].push_back({c, SIZE_MAX});
    }
  }
}

NodeId Dataflow::soleUser(ValueRef v, size_t* operand) const {
  const auto it = uses.find(v);
  if (!straight || it == uses.end() || it->second.size() != 1 || it->second[0].operand == SIZE_MAX) {
    return kInvalidId;
  }
  if (operand) *operand = it->second[0].operand;
  return it->second[0].user;
}

bool Dataflow::availableBefore(const Module& m, ValueRef v, NodeId at) const {
  const Node* def = m.find(v.id);
  if (!def) return false;
  if (def->kind == "arg" && def->scope == func) return true;
  const auto pv = position.find(v.id), pa = position.find(at);
  return pv != position.end() && pa != position.end() && pv->second < pa->second;
}

std::string epilogueStep(const Module& m, const Dataflow& df, NodeId linear, NodeId user, size_t operand,
                         std::string* why) {
  const Node* u = m.find(user);
  const std::string who = "n" + std::to_string(user) + " (" + u->kind + ")";
  if (u->kind == "aten.relu" && u->operands.size() == 1) return "relu";
  if ((u->kind == "aten.add" || u->kind == "aten.mul") && u->operands.size() == 2 && operand < 2) {
    const Operand& other = u->operands[1 - operand];
    if (other.kind != Operand::Kind::Value || !df.availableBefore(m, other.value, linear)) {
      *why = who + ": its other operand is computed after the linear, so the store can't read it";
      return "";
    }
    return u->kind.substr(5);
  }
  *why = who + " is not relu, add or mul";
  return "";
}

bool epilogueChain(const Module& m, const Dataflow& df, NodeId linear, Chain* out, std::string* err) {
  out->ops.clear();
  out->final = {linear, 0};
  const Node* n = m.find(linear);
  const AttrValue* a = n->findAttr("epilogue");
  if (!a) return true;
  if (a->kind != AttrValue::Kind::Str) {
    *err = "n" + std::to_string(linear) + " (aten.linear): epilogue must be a string like \"relu,add\"";
    return false;
  }
  for (const std::string& want : splitList(a->s)) {
    size_t operand = 0;
    const NodeId u = df.soleUser(out->final, &operand);
    std::string why;
    const std::string got = u == kInvalidId ? "" : epilogueStep(m, df, linear, u, operand, &why);
    if (got != want) {
      *err = "n" + std::to_string(linear) + " (aten.linear): epilogue step '" + want + "' no longer holds: " +
             (u == kInvalidId ? valueAtom(out->final) + " needs exactly one reader"
                              : !why.empty() ? why : "its reader is " + got);
      return false;
    }
    out->ops.push_back(u);
    out->final = {u, 0};
  }
  return true;
}

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t comma = s.find(',', start);
    const std::string item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

}  // namespace brrr::passes
