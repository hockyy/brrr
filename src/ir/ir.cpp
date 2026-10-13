#include "ir/ir.h"

#include <algorithm>
#include <cstdio>

namespace bridgeir {

AttrValue AttrValue::fromInt(int64_t v) {
  AttrValue a;
  a.kind = Kind::Int;
  a.i = v;
  return a;
}

AttrValue AttrValue::fromFloat(double v) {
  AttrValue a;
  a.kind = Kind::Float;
  a.f = v;
  return a;
}

AttrValue AttrValue::fromStr(std::string v) {
  AttrValue a;
  a.kind = Kind::Str;
  a.s = std::move(v);
  return a;
}

AttrValue AttrValue::fromSym(std::string v) {
  AttrValue a;
  a.kind = Kind::Sym;
  a.s = std::move(v);
  return a;
}

std::string AttrValue::toString() const {
  switch (kind) {
    case Kind::Int:
      return std::to_string(i);
    case Kind::Float: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", f);
      std::string s(buf);
      // Keep float-ness visible so a round trip does not flip the kind to Int.
      if (s.find_first_of(".eE") == std::string::npos) s += ".0";
      return s;
    }
    case Kind::Str: {
      std::string out = "\"";
      for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
      }
      out += '"';
      return out;
    }
    case Kind::Sym:
      return "@" + s;
  }
  return {};
}

const AttrValue* Node::findAttr(const std::string& key) const {
  auto it = attrs.find(key);
  return it == attrs.end() ? nullptr : &it->second;
}

std::string Node::attrString(const std::string& key,
                             const std::string& fallback) const {
  const AttrValue* a = findAttr(key);
  return a && a->kind == AttrValue::Kind::Sym ? a->s : fallback;
}

NodeId Module::allocId() { return next_id_++; }

Node* Module::find(NodeId id) {
  for (auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

const Node* Module::find(NodeId id) const {
  for (const auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

Node& Module::appendNode(std::string kind, NodeId scope) {
  Node n;
  n.id = allocId();
  n.kind = std::move(kind);
  n.scope = scope;
  if (scope == kInvalidId) top_level.push_back(n.id);
  nodes.push_back(std::move(n));
  return nodes.back();
}

Node& Module::insertNodeAfter(NodeId after, std::string kind, NodeId scope) {
  auto it = std::find_if(nodes.begin(), nodes.end(),
                         [&](const Node& n) { return n.id == after; });
  if (it == nodes.end()) return appendNode(std::move(kind), scope);
  Node n;
  n.id = allocId();
  n.kind = std::move(kind);
  n.scope = scope;
  if (scope == kInvalidId) top_level.push_back(n.id);
  auto pos = nodes.insert(it + 1, std::move(n));
  return *pos;
}

Node& Module::appendParsedNode(Node node) {
  if (node.id >= next_id_) next_id_ = node.id + 1;
  if (node.scope == kInvalidId) top_level.push_back(node.id);
  nodes.push_back(std::move(node));
  return nodes.back();
}

std::vector<NodeId> Module::childrenOf(NodeId scope) const {
  std::vector<NodeId> out;
  for (const auto& n : nodes)
    if (n.scope == scope) out.push_back(n.id);
  return out;
}

void Module::log(std::string action, NodeId id, std::string detail) {
  journal.push_back({std::move(action), id, std::move(detail)});
}

}  // namespace bridgeir
