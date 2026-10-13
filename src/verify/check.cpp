#include "verify/check.h"

#include <map>

#include "symbolic/expr_io.h"

namespace bridgeir::verify {
namespace {

std::string describeOperands(const schema::Kind& k) {
  if (k.operands_none) return "no operands";
  std::string s;
  for (size_t i = 0; i < k.operands.size(); ++i) {
    if (i) s += ", ";
    const bool has_type = i < k.operand_types.size() &&
                          !k.operand_types[i].empty() &&
                          k.operand_types[i] != "any";
    if (!k.operands[i].empty()) {
      s += "'" + k.operands[i] + "'";
    } else if (has_type) {
      s += k.operand_types[i];
    } else {
      s += "value";
    }
  }
  if (k.operand_tuple_width > 0) return "(" + s + ")*";
  if (k.operands_variadic) s += "*";
  return s;
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

  auto has_trait = [&](const schema::Kind* k, const char* t) {
    if (!k) return false;
    for (const std::string& x : k->traits) {
      if (x == t) return true;
    }
    return false;
  };

  // Port-kind check, driven by the declared port type.
  auto portTypeOk = [&](const ValueRef& vr, const std::string& type,
                        std::string* why) {
    const Node* t = m.find(vr.id);
    if (!t) return true;  // dangling reported by the parser
    const schema::Kind* tk = reg.find(t->kind);
    if (type == "block") {
      if (!has_trait(tk, "block")) {
        *why = "expects a block operand";
        return false;
      }
      return true;
    }
    if (type == "edge") {
      if (!has_trait(tk, "edge")) {
        *why = "expects an edge operand";
        return false;
      }
      return true;
    }
    if (type == "value") {
      if (tk && !tk->results_value) {
        *why = "expects a value-producing operand";
        return false;
      }
      return true;
    }
    return true;
  };

  for (const Node& n : m.nodes) {
    const schema::Kind* k = reg.find(n.kind);
    if (!k) continue;  // foreign kinds are reported once, below

    // Structure: counts and per-slot port kinds, proven from the declaration.
    const size_t count = n.operands.size();
    if (k->operands_specified) {
      auto checkSlots = [&](auto slot_of) {
        for (size_t i = 0; i < count; ++i) {
          const size_t slot = slot_of(i);
          if (slot >= k->operand_types.size()) continue;
          const std::string& type = k->operand_types[slot];
          if (type.empty() || type == "any") continue;
          std::string why;
          if (!portTypeOk(n.operands[i], type, &why)) {
            out.push_back({true, n.id, n.kind,
                           "operand " + std::to_string(i) + ": " + why});
          }
        }
      };

      if (k->operand_tuple_width > 0) {
        const size_t w = k->operand_tuple_width;
        if (count % w != 0) {
          out.push_back({true, n.id, n.kind,
                         "expects operands in groups of " + std::to_string(w) +
                             " (" + describeOperands(*k) + "), got " +
                             std::to_string(count)});
        } else {
          checkSlots([&](size_t i) { return i % w; });
        }
      } else if (k->operands_none) {
        if (count != 0) {
          out.push_back({true, n.id, n.kind,
                         "expects no operands, got " + std::to_string(count)});
        }
      } else if (k->operands_variadic) {
        if (count + 1 < k->operands.size()) {
          out.push_back({true, n.id, n.kind,
                         "expects " + describeOperands(*k) + ", got " +
                             std::to_string(count)});
        } else {
          checkSlots([&](size_t i) {
            const size_t last = k->operands.size() - 1;
            return i >= last ? last : i;
          });
        }
      } else {
        if (count != k->operands.size()) {
          out.push_back({true, n.id, n.kind,
                         "expects " + describeOperands(*k) + ", got " +
                             std::to_string(count)});
        } else {
          checkSlots([&](size_t i) { return i; });
        }
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

  // Edge arity (RELATIONAL): passed values must match the target block's
  // parameters. Structure and target typing are already proven generically by
  // `operands: target: block, passed: value*`.
  for (const Node& n : m.nodes) {
    const schema::Kind* k = reg.find(n.kind);
    if (!has_trait(k, "edge")) continue;
    if (n.operands.empty()) continue;  // structure check reports this
    const Node* target = m.find(n.operands[0].id);
    if (!target) continue;
    size_t params = 0;
    for (const Node& c : m.nodes) {
      if (c.scope == target->id && has_trait(reg.find(c.kind), "arg")) ++params;
    }
    const size_t passed = n.operands.size() - 1;
    if (passed != params) {
      const std::string label = target->name.empty()
                                    ? ("n" + std::to_string(target->id))
                                    : target->name;
      out.push_back({true, n.id, n.kind,
                     "passes " + std::to_string(passed) +
                         " value(s) to block '" + label + "' which takes " +
                         std::to_string(params) + " parameter(s)"});
    }
  }

  // Edge values are branch payloads: consumed by exactly one terminator, and
  // never by ordinary ops. Purely local, no CFG analysis needed.
  for (const Node& n : m.nodes) {
    if (!has_trait(reg.find(n.kind), "edge")) continue;
    size_t uses = 0;
    for (const Node& u : m.nodes) {
      const schema::Kind* uk = reg.find(u.kind);
      for (const ValueRef& vr : u.operands) {
        if (vr.id != n.id) continue;
        ++uses;
        if (!has_trait(uk, "terminator")) {
          out.push_back({true, u.id, u.kind,
                         "edge n" + std::to_string(n.id) +
                             " may only be consumed by a terminator"});
        }
      }
    }
    if (uses == 0) {
      out.push_back({false, n.id, n.kind, "edge value is never consumed"});
    } else if (uses > 1) {
      out.push_back({true, n.id, n.kind,
                     "edge value is consumed by " + std::to_string(uses) +
                         " sites (must be exactly one terminator)"});
    }
  }

  // Phi (RELATIONAL residue): pairing and from-block typing are proven by
  // `operands: (value, block)*`; what remains is predecessor membership and
  // coverage — which needs the derived predecessor relation.
  auto predecessors_of = [&](NodeId block_id) {
    std::vector<NodeId> preds;
    auto add = [&](NodeId p) {
      for (const NodeId x : preds) {
        if (x == p) return;
      }
      preds.push_back(p);
    };
    for (const Node& u : m.nodes) {
      if (!has_trait(reg.find(u.kind), "terminator")) continue;
      for (const ValueRef& vr : u.operands) {
        const Node* t = m.find(vr.id);
        if (!t) continue;
        if (has_trait(reg.find(t->kind), "block")) {
          if (t->id == block_id) add(u.scope);
        } else if (has_trait(reg.find(t->kind), "edge") && !t->operands.empty() &&
                   t->operands[0].id == block_id) {
          add(u.scope);
        }
      }
    }
    return preds;
  };

  for (const Node& n : m.nodes) {
    if (!has_trait(reg.find(n.kind), "phi")) continue;
    const std::vector<NodeId> preds = predecessors_of(n.scope);
    std::vector<NodeId> seen;
    for (size_t i = 1; i < n.operands.size(); i += 2) {
      const Node* from = m.find(n.operands[i].id);
      if (!from) continue;
      bool is_pred = false;
      for (const NodeId p : preds) {
        if (p == from->id) {
          is_pred = true;
          break;
        }
      }
      if (!is_pred) {
        const std::string label = from->name.empty()
                                      ? ("n" + std::to_string(from->id))
                                      : from->name;
        out.push_back({true, n.id, n.kind,
                       "block '" + label +
                           "' is not a predecessor of the phi's block"});
      }
      bool dup = false;
      for (const NodeId x : seen) {
        if (x == from->id) dup = true;
      }
      if (dup) {
        out.push_back({true, n.id, n.kind,
                       "duplicate from-block in phi (one pair per predecessor)"});
      }
      seen.push_back(from->id);
    }
    if (seen.size() != preds.size()) {
      out.push_back({true, n.id, n.kind,
                     "phi covers " + std::to_string(seen.size()) +
                         " predecessor(s) but the block has " +
                         std::to_string(preds.size())});
    }
  }

  // Simple structured loops (RELATIONAL): a `loop` scope carries N values as its
  // parameters, takes N initial values as operands, and has exactly one
  // `loop.yield` supplying (condition + N next values). Exits are ports.
  for (const Node& n : m.nodes) {
    if (!has_trait(reg.find(n.kind), "loop")) continue;
    size_t params = 0;
    for (const Node& c : m.nodes) {
      if (c.scope == n.id && has_trait(reg.find(c.kind), "arg")) ++params;
    }
    if (n.operands.size() != params) {
      out.push_back({true, n.id, n.kind,
                     "loop carries " + std::to_string(params) +
                         " parameter(s) but takes " +
                         std::to_string(n.operands.size()) +
                         " initial value(s)"});
    }
    const Node* yield = nullptr;
    size_t yields = 0;
    for (const Node& c : m.nodes) {
      if (c.scope == n.id && has_trait(reg.find(c.kind), "loop.yield")) {
        ++yields;
        yield = &c;
      }
    }
    if (yields != 1) {
      out.push_back({true, n.id, n.kind,
                     "loop body must contain exactly one loop.yield (found " +
                         std::to_string(yields) + ")"});
    } else if (yield->operands.size() != params + 1) {
      out.push_back({true, yield->id, yield->kind,
                     "loop.yield supplies " +
                         std::to_string(yield->operands.size()) +
                         " value(s); expected condition + " +
                         std::to_string(params) + " carried value(s)"});
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
