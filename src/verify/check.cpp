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

  // Scoping: a reference must be visible either lexically (structured scopes:
  // module/func/loop — through the scope chain) or by CFG dominance (block
  // scopes). Crossings are allowed only on sanctioned roads declared by traits:
  //   terminator -> blocks/edges (successor delivery)
  //   edge       -> its target block (operand 0)
  // There is no phi: a block's arguments, fed by edges, are the only way a value
  // crosses a block boundary.
  auto scope_reachable = [&](NodeId from_scope, NodeId target_scope) {
    NodeId s = from_scope;
    while (true) {
      if (s == target_scope) return true;
      if (s == kInvalidId) return false;
      const Node* p = m.find(s);
      s = p ? p->scope : kInvalidId;
    }
  };
  auto enclosing_with_trait = [&](NodeId scope, const char* trait) -> NodeId {
    NodeId s = scope;
    while (s != kInvalidId) {
      const Node* p = m.find(s);
      if (!p) return kInvalidId;
      if (has_trait(reg.find(p->kind), trait)) return s;
      s = p->scope;
    }
    return kInvalidId;
  };
  auto enclosing_block = [&](NodeId scope) {
    return enclosing_with_trait(scope, "block");
  };
  auto enclosing_scope_root = [&](NodeId scope) -> NodeId {
    NodeId s = scope;
    while (s != kInvalidId) {
      const Node* p = m.find(s);
      if (!p) return kInvalidId;
      if (p->kind == "func" || p->kind == "module") return s;
      s = p->scope;
    }
    return kInvalidId;
  };
  auto block_label = [&](NodeId id) {
    const Node* b = m.find(id);
    if (!b) return "n" + std::to_string(id);
    return b->name.empty() ? ("n" + std::to_string(id)) : b->name;
  };

  // Dominator sets per function (entry = first block in program order), computed
  // lazily. Only consulted for block-to-block value references.
  struct DomInfo {
    std::vector<NodeId> blocks;
    std::map<NodeId, size_t> index;
    std::vector<std::vector<char>> dom;  // dom[use][def]
  };
  std::map<NodeId, DomInfo> dom_cache;
  auto compute_dominators = [&](NodeId func_scope) {
    DomInfo info;
    for (const NodeId cid : m.childrenOf(func_scope)) {
      const Node* c = m.find(cid);
      if (c && has_trait(reg.find(c->kind), "block")) info.blocks.push_back(cid);
    }
    const size_t n = info.blocks.size();
    for (size_t i = 0; i < n; ++i) info.index[info.blocks[i]] = i;
    std::vector<std::vector<size_t>> succ(n);
    for (size_t i = 0; i < n; ++i) {
      for (const NodeId cid : m.childrenOf(info.blocks[i])) {
        const Node* c = m.find(cid);
        if (!c) continue;
        const schema::Kind* ck = reg.find(c->kind);
        if (!has_trait(ck, "terminator") && !has_trait(ck, "edge")) continue;
        for (const ValueRef& vr : c->operands) {
          const Node* tt = m.find(vr.id);
          if (!tt) continue;
          NodeId target = kInvalidId;
          if (has_trait(reg.find(tt->kind), "block")) {
            target = tt->id;
          } else if (has_trait(reg.find(tt->kind), "edge") &&
                     !tt->operands.empty()) {
            target = tt->operands[0].id;
          }
          auto it = info.index.find(target);
          if (it != info.index.end()) succ[i].push_back(it->second);
        }
      }
    }
    info.dom.assign(n, std::vector<char>(n, 1));
    if (n > 0) {
      info.dom[0].assign(n, 0);
      info.dom[0][0] = 1;
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t i = 1; i < n; ++i) {
          std::vector<size_t> preds;
          for (size_t j = 0; j < n; ++j) {
            for (const size_t s : succ[j]) {
              if (s == i) preds.push_back(j);
            }
          }
          std::vector<char> nd;
          if (preds.empty()) {
            nd.assign(n, 0);
            nd[i] = 1;
          } else {
            nd = info.dom[preds[0]];
            for (size_t k = 1; k < preds.size(); ++k) {
              for (size_t x = 0; x < n; ++x) {
                nd[x] = static_cast<char>(nd[x] && info.dom[preds[k]][x]);
              }
            }
            nd[i] = 1;
          }
          if (nd != info.dom[i]) {
            info.dom[i] = nd;
            changed = true;
          }
        }
      }
    }
    return info;
  };
  auto dominates = [&](NodeId func_scope, NodeId def_block, NodeId use_block) {
    if (def_block == use_block) return true;
    auto it = dom_cache.find(func_scope);
    if (it == dom_cache.end()) {
      it = dom_cache.emplace(func_scope, compute_dominators(func_scope)).first;
    }
    const DomInfo& info = it->second;
    const auto di = info.index.find(def_block);
    const auto ui = info.index.find(use_block);
    if (di == info.index.end() || ui == info.index.end()) return false;
    return info.dom[ui->second][di->second] != 0;
  };

  for (const Node& n : m.nodes) {
    const schema::Kind* nk = reg.find(n.kind);
    const bool is_terminator = has_trait(nk, "terminator");
    const bool is_edge = has_trait(nk, "edge");
    for (size_t oi = 0; oi < n.operands.size(); ++oi) {
      const Node* t = m.find(n.operands[oi].id);
      if (!t) continue;
      if (scope_reachable(n.scope, t->scope)) continue;
      const schema::Kind* tk = reg.find(t->kind);
      const bool sanctioned =
          (is_terminator &&
           (has_trait(tk, "block") || has_trait(tk, "edge"))) ||
          (is_edge && oi == 0);
      if (sanctioned) continue;

      const NodeId ub = enclosing_block(n.scope);
      const NodeId db = enclosing_block(t->scope);
      if (ub != kInvalidId && db != kInvalidId) {
        const NodeId f = enclosing_scope_root(ub);
        if (f != kInvalidId && f == enclosing_scope_root(db) &&
            dominates(f, db, ub)) {
          continue;  // CFG dominance makes the definition visible here
        }
        out.push_back({true, n.id, n.kind,
                       "operand n" + std::to_string(t->id) + " ('" + t->kind +
                           "') is in block '" + block_label(db) +
                           "' which does not dominate this use"});
        continue;
      }
      out.push_back({true, n.id, n.kind,
                     "operand n" + std::to_string(t->id) + " ('" + t->kind +
                         "') crosses a scope boundary; pass values through "
                         "block parameters or edges"});
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
