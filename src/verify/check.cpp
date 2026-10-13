#include "verify/check.h"

#include <map>

#include "symbolic/expr_io.h"

namespace bridgeir::verify {
namespace {

std::string describeOperands(const schema::Kind& k) {
  if (k.operands_none) return "no operands";
  std::string names;
  for (size_t i = 0; i < k.operands.size(); ++i) {
    if (i) names += ", ";
    names += "'" + k.operands[i] + "'";
  }
  if (k.operands_variadic) return names + "*";
  return names;
}

// Validate an attribute value against its declared type.
bool attrMatches(const AttrValue& v, const std::string& type,
                 const schema::Registry& reg, std::string* why) {
  if (type == "int") {
    if (v.kind != AttrValue::Kind::Int) { *why = "expects an integer"; return false; }
    return true;
  }
  if (type == "float") {
    if (v.kind != AttrValue::Kind::Float) { *why = "expects a float"; return false; }
    return true;
  }
  if (type == "str") {
    if (v.kind != AttrValue::Kind::Str) { *why = "expects a string"; return false; }
    return true;
  }
  if (type == "sym") {
    if (v.kind != AttrValue::Kind::Sym) { *why = "expects a symbol (@name)"; return false; }
    return true;
  }
  if (type == "dtype") {
    if (v.kind != AttrValue::Kind::Sym) {
      *why = "expects a scalar symbol (@f16)";
      return false;
    }
    if (!reg.findScalar(v.s)) {
      *why = "unknown scalar '" + v.s + "' (not declared in the schema)";
      return false;
    }
    return true;
  }
  if (type == "expr" || type == "dims") {
    if (v.kind != AttrValue::Kind::Str) {
      *why = "expects a quoted expression string";
      return false;
    }
    std::vector<std::string> pieces;
    if (type == "expr") {
      pieces.push_back(v.s);
    } else {
      pieces = symbolic::splitTopLevel(v.s, ',');
    }
    for (const std::string& piece : pieces) {
      std::string err;
      if (!symbolic::parseExpr(piece, &err)) {
        *why = "bad expression '" + piece + "': " + err;
        return false;
      }
    }
    return true;
  }
  *why = "unknown attribute type '" + type + "' in schema";
  return false;
}

}  // namespace

std::vector<Issue> checkModule(const Module& m, const schema::Registry& reg) {
  std::vector<Issue> out;

  for (const Node& n : m.nodes) {
    const schema::Kind* k = reg.find(n.kind);
    if (!k) continue;  // foreign kinds are reported once, below

    // Operand counts.
    if (k->operands_specified) {
      const size_t count = n.operands.size();
      bool bad = false;
      if (k->operands_none) {
        bad = count != 0;
      } else if (k->operands_variadic) {
        bad = count < k->operands.size() - 1;
      } else {
        bad = count != k->operands.size();
      }
      if (bad) {
        out.push_back({true, n.id, n.kind,
                       "expects " + describeOperands(*k) + ", got " +
                           std::to_string(count)});
      }
    }

    // Attribute policy and types.
    if (k->attr_policy == schema::Kind::AttrPolicy::None && !n.attrs.empty()) {
      out.push_back({true, n.id, n.kind, "takes no attributes"});
    }
    if (k->attr_policy == schema::Kind::AttrPolicy::Closed) {
      for (const auto& [key, val] : n.attrs) {
        auto it = k->attrs.find(key);
        if (it == k->attrs.end()) {
          out.push_back({true, n.id, n.kind,
                         "unknown attribute '" + key + "' for kind '" + n.kind +
                             "'"});
          continue;
        }
        std::string why;
        if (!attrMatches(val, it->second, reg, &why)) {
          out.push_back({true, n.id, n.kind,
                         "attribute '" + key + "': " + why});
        }
      }
      for (const auto& [key, type] : k->attrs) {
        if (!n.attrs.count(key)) {
          out.push_back({true, n.id, n.kind,
                         "missing required attribute '" + key + "' (" + type +
                             ")"});
        }
      }
    }
  }

  // Kinds that produce no value may not appear as operands.
  for (const Node& n : m.nodes) {
    for (const ValueRef& vr : n.operands) {
      const Node* t = m.find(vr.id);
      if (!t) continue;
      const schema::Kind* tk = reg.find(t->kind);
      if (tk && !tk->results_value) {
        out.push_back({true, n.id, n.kind,
                       "operand references n" + std::to_string(t->id) + " ('" +
                           t->kind + "') which produces no value"});
      }
    }
  }

  // Foreign vocabulary: reported once per kind, as a warning.
  std::map<std::string, NodeId> unknown;
  for (const Node& n : m.nodes) {
    if (!reg.find(n.kind)) unknown.emplace(n.kind, n.id);
  }
  for (const auto& [kind, id] : unknown) {
    out.push_back({false, id, kind, "unknown kind '" + kind +
                                        "' (foreign vocabulary)"});
  }

  return out;
}

bool hasErrors(const std::vector<Issue>& issues) {
  for (const Issue& i : issues) {
    if (i.error) return true;
  }
  return false;
}

}  // namespace bridgeir::verify
