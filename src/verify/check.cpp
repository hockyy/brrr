#include "verify/check.h"

#include <algorithm>
#include <map>
#include <set>

#include "symbolic/expr_io.h"
#include "symbolic/facts.h"

namespace brrr::verify {
namespace {

std::string countText(int n, const char* what) {
  return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}

bool attrMatches(const AttrValue& v, const std::string& type,
                 const schema::Registry& reg, std::string* why) {
  using K = AttrValue::Kind;
  if (type == "any") return true;
  if (type == "int" && v.kind == K::Int) return true;
  if (type == "float" && (v.kind == K::Float || v.kind == K::Int)) return true;
  if (type == "bool" && v.kind == K::Bool) return true;
  if (type == "str" && v.kind == K::Str) return true;
  if (type == "array" && v.kind == K::Array) return true;
  if (type == "type") {
    if (v.kind == K::Type) return true;
    if (v.kind == K::Ident && reg.findScalar(v.s)) return true;
    *why = v.kind == K::Ident ? "'" + v.s + "' is not a declared type" : "expects a type";
    return false;
  }
  if (type.rfind("enum(", 0) == 0) {
    const std::string inner = type.substr(5, type.size() - 6);
    std::vector<std::string> options;
    std::string cur;
    for (const char c : inner + "|") {
      if (c == '|') {
        options.push_back(cur);
        cur.clear();
      } else if (c != ' ') {
        cur += c;
      }
    }
    if (v.kind == K::Ident &&
        std::find(options.begin(), options.end(), v.s) != options.end()) {
      return true;
    }
    *why = "expects one of " + inner;
    return false;
  }
  *why = "expects " + type;
  return false;
}

// Atoms of a type (dims) that name values.
void typeUses(const Type& t, std::vector<ValueRef>* out) {
  if (!t || !t->shaped) return;
  std::set<std::string> atoms;
  for (const auto& d : t->dims) symbolic::collectVars(d, &atoms);
  for (const std::string& a : atoms) {
    ValueRef v;
    if (atomValue(a, &v)) out->push_back(v);
  }
}

class Checker {
 public:
  Checker(const Module& m, const schema::Registry& reg) : m_(m), reg_(reg) {
    for (size_t i = 0; i < m.nodes.size(); ++i) order_[m.nodes[i].id] = i;
  }

  std::vector<Issue> run() {
    for (const Node& n : m_.nodes) {
      if (schema::isStructural(n.kind)) {
        if (n.kind == "func") checkReturns(n);
        continue;
      }
      const schema::Kind* k = reg_.find(n.kind);
      if (!k) {
        foreign_.emplace(n.kind, n.id);
      } else {
        checkShape(n, *k);
        if (k->hasTrait("unrealized")) {
          warn(n.id, n.kind,
               "an unrealized cast remains (run reconcile-casts, or finish the lowering)");
        }
      }
    }
    for (const Node& n : m_.nodes) checkTypes(n);
    checkTerminators();
    checkSuccessors();
    checkLoops();
    checkVisibility();
    for (const auto& [kind, id] : foreign_) {
      warn(id, kind, "unknown kind '" + kind + "' (foreign vocabulary: not checked)");
    }
    return std::move(out_);
  }

 private:
  void error(NodeId id, const std::string& kind, const std::string& msg) {
    out_.push_back({true, id, kind, msg});
  }
  void warn(NodeId id, const std::string& kind, const std::string& msg) {
    out_.push_back({false, id, kind, msg});
  }
  const schema::Kind* kindOf(const Node& n) const { return reg_.find(n.kind); }
  bool hasTrait(const Node& n, const char* t) const {
    const schema::Kind* k = kindOf(n);
    return k && k->hasTrait(t);
  }
  std::vector<const Node*> args(NodeId owner) const {
    std::vector<const Node*> out;
    for (const NodeId c : m_.childrenOf(owner)) {
      const Node* a = m_.find(c);
      if (a && a->kind == "arg") out.push_back(a);
    }
    return out;
  }
  std::string label(const Node* b) const {
    return b ? "^" + b->symbol : std::string("?");
  }

  // ---- per-op structure, from the declaration -------------------------------

  void checkShape(const Node& n, const schema::Kind& k) {
    const int ops = static_cast<int>(n.operands.size());
    const int want = static_cast<int>(k.operands.size());
    if (k.operands_specified) {
      if (k.operands_variadic ? ops < want - 1 : ops != want) {
        error(n.id, n.kind, "expects " + countText(want, "operand") +
                                (k.operands_variadic ? " or more" : "") + ", got " +
                                std::to_string(ops));
      }
    }
    const int succ = static_cast<int>(n.successors.size());
    if (k.successors >= 0 && succ != k.successors) {
      error(n.id, n.kind, "expects " + countText(k.successors, "successor") + ", got " +
                              std::to_string(succ));
    }
    const int res = static_cast<int>(n.numResults());
    if (!k.results_variadic && res != k.results) {
      error(n.id, n.kind, "produces " + countText(k.results, "result") + ", but " +
                              std::to_string(res) + " are named");
    }
    if (n.is_scope != (k.regions > 0)) {
      error(n.id, n.kind, k.regions > 0 ? "needs a region ({ ... })" : "takes no region");
    }
    if (k.attr_policy == schema::Kind::AttrPolicy::None && !n.attrs.empty()) {
      error(n.id, n.kind, "takes no attributes");
    }
    if (k.attr_policy == schema::Kind::AttrPolicy::Closed) {
      for (const auto& [key, val] : n.attrs) {
        auto it = k.attrs.find(key);
        if (it == k.attrs.end()) {
          error(n.id, n.kind, "unknown attribute '" + key + "'");
          continue;
        }
        std::string why;
        if (!attrMatches(val, it->second.type, reg_, &why)) {
          error(n.id, n.kind, "attribute '" + key + "': " + why);
        }
      }
      for (const auto& [key, a] : k.attrs) {
        if (!a.optional && !n.attrs.count(key)) {
          error(n.id, n.kind, "missing attribute '" + key + "' (" + a.type + ")");
        }
      }
    }
  }

  void checkTypes(const Node& n) {
    for (const Type& t : n.types) {
      if (!t) continue;
      const std::string& scalar = t->shaped ? t->elem : t->name;
      if (!reg_.findScalar(scalar)) {
        warn(n.id, n.kind, "unknown scalar type '" + scalar + "' (not declared in a schema)");
      }
    }
  }

  void checkReturns(const Node& f) {
    if (f.types.empty()) return;
    for (const Node& n : m_.nodes) {
      if (n.kind != "return" || m_.namingScope(n.scope) != f.id) continue;
      if (n.operands.size() != f.types.size()) {
        error(n.id, n.kind, "returns " + std::to_string(n.operands.size()) + " value(s); @" +
                                f.symbol + " declares " + std::to_string(f.types.size()));
      }
    }
  }

  // ---- control flow ---------------------------------------------------------

  // The ops directly in a scope, in program order (args and blocks excluded).
  std::vector<const Node*> opsIn(NodeId scope) const {
    std::vector<const Node*> out;
    for (const NodeId c : m_.childrenOf(scope)) {
      const Node* n = m_.find(c);
      if (n && n->kind != "arg" && n->kind != "block") out.push_back(n);
    }
    return out;
  }

  void checkTerminators() {
    for (const Node& s : m_.nodes) {
      if (s.kind != "block" && !(s.is_scope && s.kind != "module")) continue;
      const std::vector<const Node*> ops = opsIn(s.id);
      for (size_t i = 0; i + 1 < ops.size(); ++i) {
        if (hasTrait(*ops[i], "terminator")) {
          error(ops[i]->id, ops[i]->kind, "a terminator must be the last op of its block");
        }
      }
      if (s.kind == "block") {
        if (ops.empty()) {
          error(s.id, "block", label(&s) + " is empty (it needs a terminator)");
        } else if (kindOf(*ops.back()) && !hasTrait(*ops.back(), "terminator")) {
          warn(ops.back()->id, ops.back()->kind,
               label(&s) + " does not end with a terminator");
        }
      }
    }
  }

  void checkSuccessors() {
    for (const Node& n : m_.nodes) {
      for (const Successor& s : n.successors) {
        const Node* b = m_.find(s.block);
        if (!b) continue;
        if (b->scope != regionOf(n.scope)) {
          error(n.id, n.kind, "branches to " + label(b) + ", which is outside this region");
          continue;
        }
        const size_t want = args(b->id).size();
        if (s.args.size() != want) {
          error(n.id, n.kind, "passes " + std::to_string(s.args.size()) + " value(s) to " +
                                  label(b) + ", which takes " + std::to_string(want));
        }
      }
    }
  }

  void checkLoops() {
    for (const Node& n : m_.nodes) {
      if (n.kind == "continue_if") {
        const Node* r = m_.find(regionOf(n.scope));
        if (!r || !hasTrait(*r, "loop")) {
          error(n.id, n.kind, "continue_if is only allowed in a loop region");
        }
      }
      if (!hasTrait(n, "loop")) continue;
      // Carried values: the entry block's arguments (`^iter(%acc: i64):`).
      const Node* entry = nullptr;
      for (const NodeId c : m_.childrenOf(n.id)) {
        const Node* cn = m_.find(c);
        if (cn && cn->kind == "block") {
          entry = cn;
          break;
        }
      }
      const size_t carried = entry ? args(entry->id).size() : 0;
      if (n.operands.size() != carried) {
        error(n.id, n.kind, "carries " + std::to_string(carried) + " value(s) but takes " +
                                std::to_string(n.operands.size()) + " initial value(s)");
      }
      if (n.numResults() != carried) {
        error(n.id, n.kind, "has " + std::to_string(n.numResults()) + " result(s) for " +
                                std::to_string(carried) + " carried value(s)");
      }
      size_t continues = 0;
      for (const Node& c : m_.nodes) {
        if (c.kind != "continue_if" || regionOf(c.scope) != n.id) continue;
        ++continues;
        if (c.operands.size() != carried + 1) {
          error(c.id, c.kind, "supplies " + std::to_string(c.operands.size()) +
                                  " value(s); expected a condition + " +
                                  std::to_string(carried) + " carried value(s)");
        }
      }
      if (continues == 0) error(n.id, n.kind, "the loop region has no continue_if");
    }
  }

  // ---- visibility -----------------------------------------------------------

  // The function or region op that owns `scope` (skipping a block).
  NodeId regionOf(NodeId scope) const {
    const Node* s = m_.find(scope);
    return s && s->kind == "block" ? s->scope : scope;
  }

  struct Cfg {
    std::vector<NodeId> nodes;  // [region (its entry), blocks...]
    std::map<NodeId, size_t> index;
    std::vector<std::vector<char>> dom;  // dom[x][y]: y dominates x
  };

  const Cfg& cfg(NodeId region) {
    auto it = cfgs_.find(region);
    if (it != cfgs_.end()) return it->second;
    Cfg c;
    c.nodes.push_back(region);
    for (const NodeId ch : m_.childrenOf(region)) {
      const Node* b = m_.find(ch);
      if (b && b->kind == "block") c.nodes.push_back(ch);
    }
    const size_t n = c.nodes.size();
    for (size_t i = 0; i < n; ++i) c.index[c.nodes[i]] = i;
    std::vector<std::vector<size_t>> preds(n);
    for (size_t i = 0; i < n; ++i) {
      for (const Node* op : opsIn(c.nodes[i])) {
        for (const Successor& s : op->successors) {
          auto jt = c.index.find(s.block);
          if (jt != c.index.end()) preds[jt->second].push_back(i);
        }
      }
    }
    c.dom.assign(n, std::vector<char>(n, 1));
    c.dom[0].assign(n, 0);
    c.dom[0][0] = 1;
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t i = 1; i < n; ++i) {
        std::vector<char> nd(n, 0);
        if (!preds[i].empty()) {
          nd = c.dom[preds[i][0]];
          for (size_t k = 1; k < preds[i].size(); ++k) {
            for (size_t x = 0; x < n; ++x) nd[x] = nd[x] && c.dom[preds[i][k]][x];
          }
        }
        nd[i] = 1;
        if (nd != c.dom[i]) {
          c.dom[i] = nd;
          changed = true;
        }
      }
    }
    return cfgs_.emplace(region, std::move(c)).first->second;
  }

  // Is `def` visible at a use inside scope `use_scope` (by the node `user`)?
  // Strict program order applies only within one scope, and not for facts.
  bool visible(const Node& def, NodeId use_scope, const Node* user) {
    // Ancestor scope: lexically visible (inner scopes see outer values).
    for (NodeId s = use_scope;; s = m_.find(s)->scope) {
      if (s == def.scope) {
        // Program order binds within one scope; an argument is visible to the
        // whole body, but only to the arguments declared after it.
        if (user && s == user->scope && (def.kind != "arg" || user->kind == "arg")) {
          return order_[def.id] < order_[user->id];
        }
        return true;
      }
      if (s == kInvalidId || !m_.find(s)) break;
    }
    // CFG dominance: the def's block must dominate the use's block in the same region.
    const Node* db = m_.find(def.scope);
    if (!db || db->kind != "block") return false;
    const NodeId region = db->scope;
    NodeId ub = kInvalidId;
    for (NodeId s = use_scope; s != kInvalidId;) {
      const Node* sn = m_.find(s);
      if (!sn) break;
      if (sn->scope == region) {
        ub = s;
        break;
      }
      s = sn->scope;
    }
    if (ub == kInvalidId) return false;
    const Cfg& c = cfg(region);
    auto ui = c.index.find(ub);
    auto di = c.index.find(db->id);
    if (ui == c.index.end() || di == c.index.end()) return false;  // ub is an op: entry
    return c.dom[ui->second][di->second] != 0;
  }

  void checkUse(const Node& user, NodeId scope, ValueRef v, const char* where,
                bool ordered = true) {
    const Node* def = m_.find(v.id);
    if (!def) return;  // the parser reports unknown values
    if (visible(*def, scope, ordered ? &user : nullptr)) return;
    const Node* db = m_.find(def->scope);
    std::string why = db && db->kind == "block"
                          ? "is defined in " + label(db) + ", which does not dominate this use"
                          : "is not visible here (defined later, or in another scope)";
    std::string name = valueAtom(v);
    if (v.port < def->names.size() && !def->names[v.port].empty()) name = "%" + def->names[v.port];
    error(user.id, user.kind, std::string(where) + " " + name + " " + why);
  }

  void checkVisibility() {
    for (const Node& n : m_.nodes) {
      if (n.kind == "func" || n.kind == "module") continue;
      for (const Operand& o : n.operands) {
        std::vector<ValueRef> us;
        o.uses(&us);
        for (const ValueRef& v : us) checkUse(n, n.scope, v, "operand");
      }
      for (const Successor& s : n.successors) {
        for (const ValueRef& v : s.args) checkUse(n, n.scope, v, "successor argument");
      }
      std::vector<ValueRef> tus;
      for (const Type& t : n.types) typeUses(t, &tus);
      for (const ValueRef& v : tus) {
        if (v.id == n.id) {
          error(n.id, n.kind, "a type cannot mention its own value");
          continue;
        }
        checkUse(n, n.scope, v, "type dimension");
      }
    }
    for (const Module::FactEntry& f : m_.facts) {
      symbolic::ParsedFact pf;
      std::string err;
      if (!symbolic::parseFact(f.text, &pf, &err)) continue;
      std::set<std::string> atoms;
      symbolic::collectFactVars(pf, &atoms);
      for (const std::string& a : atoms) {
        ValueRef v;
        if (!atomValue(a, &v)) continue;
        const Node* def = m_.find(v.id);
        if (def && !visible(*def, f.scope, nullptr)) {
          error(f.scope, "fact", "fact mentions " + a + ", which is not visible in its scope");
        }
      }
    }
  }

  const Module& m_;
  const schema::Registry& reg_;
  std::vector<Issue> out_;
  std::map<std::string, NodeId> foreign_;
  std::map<NodeId, size_t> order_;
  std::map<NodeId, Cfg> cfgs_;
};

}  // namespace

std::vector<Issue> checkModule(const Module& m, const schema::Registry& reg) {
  return Checker(m, reg).run();
}

bool hasErrors(const std::vector<Issue>& issues) {
  return std::any_of(issues.begin(), issues.end(), [](const Issue& i) { return i.error; });
}

}  // namespace brrr::verify
