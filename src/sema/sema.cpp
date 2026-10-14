#include "sema/sema.h"

#include <algorithm>
#include <cctype>
#include <climits>

#include "symbolic/expr_io.h"
#include "symbolic/facts.h"

namespace brrr::sema {
namespace {

using symbolic::Expr;
using symbolic::ExprTerm;
using symbolic::Tri;

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

Expr substAtoms(const Expr& e, const std::map<std::string, Expr>& m) {
  if (e->kind == ExprTerm::Kind::Const) return e;
  if (e->kind == ExprTerm::Kind::Var) {
    auto it = m.find(e->name);
    return it == m.end() ? e : it->second;
  }
  std::vector<Expr> args;
  for (const Expr& a : e->args) args.push_back(substAtoms(a, m));
  return ExprTerm::rebuild(e, std::move(args));
}

Expr product(const std::vector<Expr>& dims) {
  if (dims.empty()) return ExprTerm::constant(1);
  return dims.size() == 1 ? dims[0] : ExprTerm::mul(dims);
}

bool isUpper(const std::string& s) {
  return !s.empty() && std::isupper(static_cast<unsigned char>(s[0]));
}

}  // namespace

Sema::Sema(const Module& m, const schema::Registry& reg) : m_(m), reg_(reg) {}

// ---- types ------------------------------------------------------------------

// Each `?` becomes dim(%owner, k): a fresh dimension with identity.
Type Sema::freshen(const Type& t, ValueRef owner) const {
  if (!t || !t->shaped) return t;
  auto t2 = std::make_shared<TypeTerm>(*t);
  for (size_t k = 0; k < t2->dims.size(); ++k) {
    if (t2->dims[k]->kind == ExprTerm::Kind::Var && t2->dims[k]->name == "?") {
      t2->dims[k] = ExprTerm::var(dimAtom(owner, k));
    }
  }
  return t2;
}

// The range of an integer type: iN signed, uN unsigned, index signed 64-bit.
// Untyped values reason like index.
bool Sema::intRange(const Type& t, __int128* lo, __int128* hi) const {
  if (!t) {
    *lo = INT64_MIN;
    *hi = INT64_MAX;
    return true;
  }
  const schema::Scalar* s = t->shaped ? nullptr : reg_.findScalar(t->name);
  if (!s || s->bits <= 0 || s->bits > 64) return false;
  if (s->domain == "index") {
    *lo = INT64_MIN;
    *hi = INT64_MAX;
  } else if (s->domain == "uint") {
    *lo = 0;
    *hi = (static_cast<__int128>(1) << s->bits) - 1;
  } else if (s->domain == "int") {
    *lo = -(static_cast<__int128>(1) << (s->bits - 1));
    *hi = (static_cast<__int128>(1) << (s->bits - 1)) - 1;
  } else {
    return false;
  }
  return true;
}

bool Sema::fitsIn(const Type& from, const Type& to) const {
  __int128 flo, fhi, tlo, thi;
  return intRange(from, &flo, &fhi) && intRange(to, &tlo, &thi) && tlo <= flo && fhi <= thi;
}

bool Sema::isIntegerType(const Type& t) const {
  if (!t) return true;  // gradual: untyped values reason like index
  if (t->shaped) return false;
  const schema::Scalar* s = reg_.findScalar(t->name);
  return s && (s->domain == "int" || s->domain == "uint" || s->domain == "index");
}

Type Sema::typeOf(ValueRef v) {
  auto it = types_.find(v);
  if (it != types_.end()) return it->second;
  const Node* n = m_.find(v.id);
  if (!n || in_progress_.count(v)) return nullptr;
  Type t = freshen(n->typeOf(v.port), v);
  if (!t && v.port == 0 && !schema::isStructural(n->kind)) {
    const schema::Kind* k = reg_.find(n->kind);
    if (k && !k->result_type.empty()) {  // `type: index` in the schema
      auto fixed = std::make_shared<TypeTerm>();
      fixed->name = k->result_type;
      t = fixed;
    } else {
      in_progress_.insert(v);
      t = applyRule(*n, nullptr, nullptr);
      in_progress_.erase(v);
    }
    if (t) inferred_.insert(v);
  }
  types_[v] = t;
  return t;
}

bool Sema::isInferred(ValueRef v) {
  typeOf(v);
  return inferred_.count(v) > 0;
}

// ---- meanings ---------------------------------------------------------------

Expr Sema::meaning(ValueRef v) {
  auto it = meanings_.find(v);
  if (it != meanings_.end()) return it->second;
  const Expr self = ExprTerm::var(valueAtom(v));
  const Node* n = m_.find(v.id);
  const schema::Kind* k = n ? reg_.find(n->kind) : nullptr;
  if (!k || k->expr.empty() || v.port != 0 || in_progress_meaning_.count(v)) {
    return meanings_[v] = self;
  }

  // Only integers that cannot wrap get a meaning (see sema.h). A literal or a
  // dimension cannot wrap; arithmetic on iN/uN needs the op's no-wrap flag.
  // A conversion means its operand only when every source value fits the target
  // type (i32 -> i64, u16 -> i32, i64 -> index); i32 -> u32 may wrap.
  const std::string& tpl = k->expr;
  const bool conversion = k->hasTrait("conversion");
  const bool arithmetic = !conversion && tpl[0] != '@' && tpl.rfind("dim(", 0) != 0;
  const Type rt = typeOf(v);
  if (!isIntegerType(rt)) return meanings_[v] = self;
  if (conversion) {
    const Operand* src = n->operands.empty() ? nullptr : &n->operands[0];
    const Type st = src && src->kind == Operand::Kind::Value ? typeOf(src->value) : nullptr;
    if (!st || !isIntegerType(st) || !fitsIn(st, rt)) return meanings_[v] = self;
  }
  const schema::Scalar* rs = rt ? reg_.findScalar(rt->name) : nullptr;
  if (arithmetic && rs && rs->domain != "index") {
    const AttrValue* a = n->findAttr(rs->domain == "uint" ? "nuw" : "nsw");
    if (!(a && a->kind == AttrValue::Kind::Bool && a->b)) return meanings_[v] = self;
  }

  in_progress_meaning_.insert(v);
  Expr result = self;
  auto attrInt = [&](const std::string& name, int64_t* out) {
    const AttrValue* a = n->findAttr(name);
    if (!a) return false;
    if (a->kind == AttrValue::Kind::Int) {
      *out = a->i;
      return true;
    }
    return false;
  };
  auto slotIndex = [&](const std::string& slot) -> int {
    for (size_t i = 0; i < k->operands.size(); ++i) {
      if (k->operands[i] == slot) return static_cast<int>(i);
    }
    return -1;
  };
  auto operandExpr = [&](int i) -> Expr {
    if (i < 0 || static_cast<size_t>(i) >= n->operands.size()) return nullptr;
    const Operand& o = n->operands[i];
    if (o.kind == Operand::Kind::Value) {
      const Node* d = m_.find(o.value.id);
      const Type ot = typeOf(o.value);
      if (!isIntegerType(ot) || (d && d->kind == "block")) return nullptr;
      return ExprTerm::var(valueAtom(o.value));
    }
    return o.kind == Operand::Kind::Expr ? o.exprs[0] : nullptr;
  };

  if (tpl.size() > 1 && tpl[0] == '@') {  // `@value`: an integer attribute
    int64_t c = 0;
    if (attrInt(tpl.substr(1), &c)) result = ExprTerm::constant(c);
  } else if (tpl.rfind("dim(", 0) == 0 && tpl.back() == ')') {  // dim(slot, @attr)
    const auto parts = symbolic::splitTopLevel(tpl.substr(4, tpl.size() - 5), ',');
    int64_t index = -1;
    const int slot = parts.size() == 2 ? slotIndex(trim(parts[0])) : -1;
    if (slot >= 0 && static_cast<size_t>(slot) < n->operands.size() &&
        parts[1].size() > 1 && parts[1][0] == '@' && attrInt(trim(parts[1]).substr(1), &index) &&
        n->operands[slot].kind == Operand::Kind::Value) {
      const Type st = typeOf(n->operands[slot].value);
      if (st && st->shaped && index >= 0 && static_cast<size_t>(index) < st->dims.size()) {
        result = st->dims[index];
      }
    }
  } else {  // an expression over operand slots
    std::string err;
    const Expr e = symbolic::parseExpr(tpl, &err);
    if (e) {
      std::set<std::string> slots;
      symbolic::collectVars(e, &slots);
      std::map<std::string, Expr> sub;
      bool ok = true;
      for (const std::string& s : slots) {
        const Expr oe = operandExpr(slotIndex(s));
        if (!oe) {
          ok = false;
          break;
        }
        sub[s] = oe;
      }
      if (ok) result = substAtoms(e, sub);
    }
  }
  in_progress_meaning_.erase(v);
  return meanings_[v] = result;
}

// ---- solver -----------------------------------------------------------------

const symbolic::Solver& Sema::solverFor(NodeId scope) {
  auto it = solvers_.find(scope);
  if (it != solvers_.end()) return it->second;
  return solvers_.emplace(scope, build(scope, m_.facts.size())).first->second;
}

symbolic::Expr Sema::operandExpr(const Operand& o) {
  if (o.kind == Operand::Kind::Value) return ExprTerm::var(valueAtom(o.value));
  return o.kind == Operand::Kind::Expr ? o.exprs[0] : nullptr;
}

// Definitions, type ranges, guards, and the facts on the chain root -> scope —
// except facts in `scope` itself from index `stop_fact` on (to prove a lemma
// from what precedes it).
symbolic::Solver Sema::build(NodeId scope, size_t stop_fact) {
  if (!defs_ready_) {
    for (const Node& n : m_.nodes) {
      if (schema::isStructural(n.kind)) continue;
      for (uint32_t p = 0; p < n.numResults(); ++p) {
        const ValueRef v{n.id, p};
        const Expr e = meaning(v);
        if (!(e->kind == ExprTerm::Kind::Var && e->name == valueAtom(v))) {
          defs_.emplace_back(ExprTerm::var(valueAtom(v)), e);
        }
      }
    }
    // dim(%x, k) is the k-th dimension of %x's type, whatever that type says.
    for (const Node& n : m_.nodes) {
      if (n.kind == "func" || n.kind == "module" || n.kind == "block") continue;
      for (uint32_t p = 0; p < n.numResults(); ++p) {
        const Type t = typeOf({n.id, p});
        if (!t || !t->shaped) continue;
        for (size_t k = 0; k < t->dims.size(); ++k) {
          const std::string atom = dimAtom({n.id, p}, k);
          const Expr& d = t->dims[k];
          if (!(d->kind == ExprTerm::Kind::Var && d->name == atom)) {
            defs_.emplace_back(ExprTerm::var(atom), d);
          }
        }
      }
    }
    defs_ready_ = true;
  }
  symbolic::Solver s;
  for (const auto& [a, b] : defs_) s.assertEqual(a, b);
  // Typed integer atoms range over their type (widths below 64 bits).
  for (const Node& n : m_.nodes) {
    for (uint32_t p = 0; p < n.numResults(); ++p) {
      const Type t = n.typeOf(p);
      if (!t || t->shaped) continue;
      const schema::Scalar* sc = reg_.findScalar(t->name);
      if (!sc || sc->bits <= 0 || sc->bits >= 64) continue;
      if (sc->domain == "uint") {
        s.assertRange(valueAtom({n.id, p}), 0, (int64_t{1} << sc->bits) - 1);
      } else if (sc->domain == "int") {
        s.assertRange(valueAtom({n.id, p}), -(int64_t{1} << (sc->bits - 1)),
                      (int64_t{1} << (sc->bits - 1)) - 1);
      }
    }
  }
  // A tensor's dimensions are non-negative. Assumed for arguments (whoever passes
  // the tensor made it) and for fresh dimensions (an op's own `?` is a size);
  // every other dimension is proven in check().
  const Expr zero = ExprTerm::constant(0);
  for (const Node& n : m_.nodes) {
    if (n.kind == "func" || n.kind == "module" || n.kind == "block") continue;
    for (uint32_t p = 0; p < n.numResults(); ++p) {
      const ValueRef v{n.id, p};
      const Type t = typeOf(v);
      if (!t || !t->shaped) continue;
      for (size_t k = 0; k < t->dims.size(); ++k) {
        const Expr& d = t->dims[k];
        const bool own = d->kind == ExprTerm::Kind::Var && d->name == dimAtom(v, k);
        if (n.kind == "arg" || own) s.assertLe(zero, d);
      }
    }
  }
  std::vector<NodeId> chain;
  for (NodeId x = scope; x != kInvalidId;) {
    chain.insert(chain.begin(), x);
    const Node* nx = m_.find(x);
    x = nx ? nx->scope : kInvalidId;
  }
  chain.insert(chain.begin(), kInvalidId);
  // Values are SSA: a fact that holds in a block holds in every block it
  // dominates. So the innermost block's dominators contribute their facts too.
  // Sound by induction for loops: the first entry into a block D comes from a
  // block D does not dominate, and a later entry (a back edge from a block D
  // dominates) may assume what held on the previous entry.
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const Node* nb = m_.find(*it);
    if (!nb || nb->kind != "block") continue;
    const std::vector<NodeId> doms = dominatingBlocks(*it);
    chain.insert(chain.begin() + 1, doms.begin(), doms.end());
    break;
  }
  for (const NodeId sc : chain) {
    for (size_t i = 0; i < m_.facts.size(); ++i) {
      const Module::FactEntry& f = m_.facts[i];
      if (f.scope != sc || (sc == scope && i >= stop_fact)) continue;
      std::string err;
      if (!symbolic::applyFact(f.text, s, &err)) fact_issues_.push_back({true, sc, "fact", err});
    }
    // A guard is a checked fact: below it, its relation holds.
    for (const Node& g : m_.nodes) {
      if (g.kind != "guard" || g.scope != sc || g.operands.size() != 2) continue;
      const Expr a = operandExpr(g.operands[0]);
      const Expr b = operandExpr(g.operands[1]);
      const AttrValue* rel = g.findAttr("rel");
      if (!a || !b || !rel) continue;
      if (rel->s == "eq") s.assertEqual(a, b);
      if (rel->s == "le") s.assertLe(a, b);
    }
  }
  return s;
}

const std::vector<NodeId>& Sema::dominatingBlocks(NodeId block) {
  if (const auto it = dominators_.find(block); it != dominators_.end()) return it->second;
  const Node* b = m_.find(block);
  const NodeId fn = b ? b->scope : kInvalidId;  // a block's scope is its function
  // The function's CFG: its own scope is the entry, then every block.
  std::vector<NodeId> nodes{fn};
  for (NodeId c : m_.childrenOf(fn)) {
    if (const Node* n = m_.find(c); n && n->kind == "block") nodes.push_back(c);
  }
  std::map<NodeId, std::vector<NodeId>> preds;
  for (const NodeId from : nodes) {
    for (NodeId c : m_.childrenOf(from)) {
      const Node* n = m_.find(c);
      if (!n) continue;
      for (const Successor& s : n->successors) preds[s.block].push_back(from);
    }
  }
  // Iterative dominator sets: Dom(entry) = {entry}; Dom(b) = {b} + the
  // intersection over b's predecessors. Unreachable blocks keep only themselves.
  const std::set<NodeId> all(nodes.begin(), nodes.end());
  std::map<NodeId, std::set<NodeId>> dom;
  for (const NodeId n : nodes) dom[n] = n == fn || preds[n].empty() ? std::set<NodeId>{n} : all;
  for (bool changed = true; changed;) {
    changed = false;
    for (const NodeId n : nodes) {
      if (n == fn || preds[n].empty()) continue;
      std::set<NodeId> d = dom[preds[n][0]];
      for (const NodeId p : preds[n]) {
        std::set<NodeId> keep;
        for (const NodeId x : d) {
          if (dom[p].count(x)) keep.insert(x);
        }
        d = std::move(keep);
      }
      d.insert(n);
      if (d != dom[n]) {
        dom[n] = std::move(d);
        changed = true;
      }
    }
  }
  for (const NodeId n : nodes) {
    std::vector<NodeId> strict;
    for (const NodeId d : nodes) {  // program order
      if (d != n && d != fn && dom[n].count(d)) strict.push_back(d);
    }
    dominators_[n] = std::move(strict);
  }
  return dominators_[block];
}

// The relation a branch condition implies on the edge taken when cond == polarity.
void Sema::addCondition(symbolic::Solver& s, const Operand& cond, bool polarity) {
  if (cond.kind != Operand::Kind::Value) return;
  const Node* c = m_.find(cond.value.id);
  if (!c || c->kind != "icmp" || c->operands.size() != 2) return;
  const AttrValue* pred = c->findAttr("pred");
  const Expr l = operandExpr(c->operands[0]);
  const Expr r = operandExpr(c->operands[1]);
  if (!pred || !l || !r) return;
  for (const Operand& o : c->operands) {  // integers only
    if (o.kind == Operand::Kind::Value && !isIntegerType(typeOf(o.value))) return;
  }
  auto plus1 = [](const Expr& e) { return ExprTerm::add({e, ExprTerm::constant(1)}); };
  std::string p = pred->s;
  if (!polarity) {  // the false edge holds the negation
    static const std::map<std::string, std::string> neg = {
        {"eq", "ne"}, {"ne", "eq"}, {"lt", "ge"}, {"ge", "lt"}, {"gt", "le"}, {"le", "gt"}};
    auto it = neg.find(p);
    if (it == neg.end()) return;
    p = it->second;
  }
  if (p == "eq") s.assertEqual(l, r);
  if (p == "le") s.assertLe(l, r);
  if (p == "lt") s.assertLe(plus1(l), r);
  if (p == "ge") s.assertLe(r, l);
  if (p == "gt") s.assertLe(plus1(r), l);
}

std::vector<Sema::Edge> Sema::edgesInto(const Node& block) {
  std::vector<Edge> out;
  auto where = [&](const Node& op) {
    const Node* s = m_.find(op.scope);
    return s && s->kind == "block" ? "^" + s->symbol : std::string("entry");
  };
  for (const Node& t : m_.nodes) {
    for (size_t k = 0; k < t.successors.size(); ++k) {
      if (t.successors[k].block != block.id) continue;
      Edge e;
      e.from = &t;
      e.scope = t.scope;
      for (const ValueRef& v : t.successors[k].args) e.passed.push_back(ExprTerm::var(valueAtom(v)));
      if (t.kind == "cond_br" && !t.operands.empty()) {
        e.cond = &t.operands[0];
        e.polarity = k == 0;
      }
      e.label = "from " + where(t) + (e.cond ? (k == 0 ? " (true edge)" : " (false edge)") : "");
      out.push_back(e);
    }
  }
  // The entry block of a loop region: the initial values, then each back edge.
  const Node* loop = m_.find(block.scope);
  const schema::Kind* lk = loop ? reg_.find(loop->kind) : nullptr;
  if (lk && lk->hasTrait("loop")) {
    for (const NodeId c : m_.childrenOf(loop->id)) {
      const Node* first = m_.find(c);
      if (!first || first->kind != "block") continue;
      if (first->id != block.id) break;
      Edge init;
      init.from = loop;
      init.scope = loop->scope;
      for (const Operand& o : loop->operands) init.passed.push_back(operandExpr(o));
      init.label = "on loop entry";
      out.push_back(init);
      for (const Node& ci : m_.nodes) {
        if (ci.kind != "continue_if" || ci.operands.empty()) continue;
        const Node* s = m_.find(ci.scope);
        if (ci.scope != loop->id && !(s && s->scope == loop->id)) continue;
        Edge back;
        back.from = &ci;
        back.scope = ci.scope;
        for (size_t i = 1; i < ci.operands.size(); ++i) back.passed.push_back(operandExpr(ci.operands[i]));
        back.cond = &ci.operands[0];
        back.label = "on the back edge";
        out.push_back(back);
      }
      break;
    }
  }
  return out;
}

void Sema::checkFacts(std::vector<verify::Issue>* out) {
  struct Piece {
    std::string op;  // "=", "<=", "aligned", "range"
    Expr a, b;
    int64_t m = 0, lo = 0, hi = 0;
  };
  auto prove = [&](const symbolic::Solver& s, const Piece& p) -> Tri {
    if (p.op == "=") return s.isEqual(p.a, p.b);
    if (p.op == "<=") return s.lessOrEqual(p.a, p.b);
    if (p.op == "aligned") return s.aligned(p.a, p.m);
    const Tri lo = s.lessOrEqual(ExprTerm::constant(p.lo), p.a);
    const Tri hi = s.lessOrEqual(p.a, ExprTerm::constant(p.hi));
    if (lo == Tri::No || hi == Tri::No) return Tri::No;
    return lo == Tri::Yes && hi == Tri::Yes ? Tri::Yes : Tri::Unknown;
  };
  auto text = [&](const Piece& p) {
    if (p.op == "aligned") return "aligned(" + show(p.a) + ", " + std::to_string(p.m) + ")";
    if (p.op == "range") {
      return "range(" + show(p.a) + ", " + std::to_string(p.lo) + ", " + std::to_string(p.hi) + ")";
    }
    return show(p.a) + " " + p.op + " " + show(p.b);
  };

  for (size_t fi = 0; fi < m_.facts.size(); ++fi) {
    const Module::FactEntry& f = m_.facts[fi];
    const Node* scope = m_.find(f.scope);
    if (!scope || scope->kind == "module") continue;  // assumptions
    symbolic::ParsedFact pf;
    std::string err;
    if (!symbolic::parseFact(f.text, &pf, &err)) continue;
    std::vector<Piece> pieces;
    if (pf.kind == symbolic::ParsedFact::Kind::Aligned) {
      Piece p;
      p.op = "aligned";
      p.a = pf.exprs[0];
      p.m = pf.a;
      pieces.push_back(p);
    } else if (pf.kind == symbolic::ParsedFact::Kind::Range) {
      Piece p;
      p.op = "range";
      p.a = pf.exprs[0];
      p.lo = pf.a;
      p.hi = pf.b;
      pieces.push_back(p);
    } else {
      for (size_t k = 0; k < pf.ops.size(); ++k) {
        const bool ge = pf.ops[k] == ">=";
        Piece p;
        p.op = ge ? "<=" : pf.ops[k];
        p.a = ge ? pf.exprs[k + 1] : pf.exprs[k];
        p.b = ge ? pf.exprs[k] : pf.exprs[k + 1];
        pieces.push_back(p);
      }
    }
    for (const Piece& p : pieces) {
      // A lemma mentions an op result of this very scope; otherwise the fact is
      // about what flows in (arguments and outer values).
      std::set<std::string> atoms;
      symbolic::collectVars(p.a, &atoms);
      if (p.b) symbolic::collectVars(p.b, &atoms);
      bool lemma = false;
      for (const std::string& a : atoms) {
        ValueRef v;
        const Node* d = atomValue(a, &v) ? m_.find(v.id) : nullptr;
        if (d && d->kind != "arg" && d->scope == f.scope) lemma = true;
      }
      if (lemma) {
        const Tri t = prove(build(f.scope, fi), p);
        if (t == Tri::No) {
          out->push_back({true, f.scope, "fact", "fact " + text(p) + " is false"});
        } else if (t == Tri::Unknown) {
          out->push_back({false, f.scope, "fact",
                          "fact " + text(p) + " does not follow from what precedes it"});
        }
        continue;
      }
      if (scope->kind != "block") continue;  // function facts on arguments: preconditions
      std::vector<const Node*> args;
      for (const NodeId c : m_.childrenOf(scope->id)) {
        const Node* a = m_.find(c);
        if (a && a->kind == "arg") args.push_back(a);
      }
      for (const Edge& e : edgesInto(*scope)) {
        if (e.passed.size() != args.size()) continue;  // the checker reports arity
        std::map<std::string, Expr> sub;
        for (size_t i = 0; i < args.size(); ++i) {
          if (e.passed[i]) sub[valueAtom({args[i]->id, 0})] = e.passed[i];
        }
        Piece q = p;
        q.a = substAtoms(p.a, sub);
        if (p.b) q.b = substAtoms(p.b, sub);
        symbolic::Solver s = solverFor(e.scope);
        if (e.cond) addCondition(s, *e.cond, e.polarity);
        const Tri t = prove(s, q);
        if (t == Tri::Yes) continue;
        const std::string msg = "invariant " + text(p) + " of ^" + scope->symbol + " " + e.label +
                                ": " + text(q);
        if (t == Tri::No) {
          out->push_back({true, e.from->id, e.from->kind, msg + " is false"});
        } else {
          out->push_back({false, e.from->id, e.from->kind, msg + " is not proven"});
        }
      }
    }
  }
}

// ---- shape rules ------------------------------------------------------------

const Sema::Rule& Sema::ruleFor(const schema::Kind& k) {
  auto it = rules_.find(k.name);
  if (it != rules_.end()) return it->second;
  Rule r;
  auto parsePattern = [](const std::string& text, Pattern* p) {
    const std::string t = trim(text);
    if (t.empty()) return false;
    if (t.front() == '[' && t.back() == ']') {
      for (const std::string& d : symbolic::splitTopLevel(t.substr(1, t.size() - 2), ',')) {
        if (d.empty()) return false;
        p->dims.push_back(d);
      }
      return true;
    }
    if (!isUpper(t)) return false;
    p->whole = true;
    p->var = t;
    return true;
  };
  const size_t arrow = k.shape.find("->");
  if (arrow != std::string::npos) {
    r.ok = true;
    for (const std::string& in : symbolic::splitTopLevel(k.shape.substr(0, arrow), ',')) {
      Pattern p;
      r.ok = r.ok && parsePattern(in, &p);
      r.ins.push_back(p);
    }
    std::string out = k.shape.substr(arrow + 2);
    const size_t colon = out.find(':');
    if (colon != std::string::npos) {
      r.out_elem = trim(out.substr(colon + 1));
      out = out.substr(0, colon);
    }
    r.ok = r.ok && parsePattern(out, &r.out);
  }
  return rules_.emplace(k.name, std::move(r)).first->second;
}

Type Sema::applyRule(const Node& n, std::vector<Obligation>* obs,
                     std::vector<verify::Issue>* issues) {
  const schema::Kind* k = reg_.find(n.kind);
  if (!k || k->shape.empty() || n.numResults() != 1) return nullptr;
  const Rule& r = ruleFor(*k);
  if (!r.ok || r.ins.size() != n.operands.size()) return nullptr;

  // Elementwise rules (`S, S -> S`) also type scalars: operands of one scalar
  // type give that type; an expression operand (`sub %i, 1`) adapts to it.
  bool elementwise = r.out.whole;
  for (const Pattern& p : r.ins) elementwise = elementwise && p.whole && p.var == r.out.var;
  Type scalar;
  bool has_shaped = false;
  for (const Operand& o : n.operands) {
    if (o.kind != Operand::Kind::Value) continue;
    const Type t = typeOf(o.value);
    if (!t) return nullptr;  // gradual: untyped operands, no judgment
    if (t->shaped) {
      has_shaped = true;
    } else if (!scalar) {
      scalar = t;
    } else if (scalar->name != t->name && issues) {
      issues->push_back({true, n.id, n.kind,
                         "operands have different types: " + scalar->name + " and " + t->name});
      return nullptr;
    }
  }
  if (!has_shaped) {
    if (!elementwise || !scalar) return nullptr;
    if (const AttrValue* to = n.findAttr("to")) {  // a conversion names its result type
      if (to->kind == AttrValue::Kind::Ident) {
        auto t = std::make_shared<TypeTerm>();
        t->name = to->s;
        return t;
      }
      if (to->kind == AttrValue::Kind::Type && to->type && !to->type->shaped) return to->type;
    }
    return scalar;
  }

  std::vector<Type> ins;
  for (const Operand& o : n.operands) {
    if (o.kind != Operand::Kind::Value) return nullptr;
    const Type t = typeOf(o.value);
    if (!t || !t->shaped) return nullptr;
    ins.push_back(t);
  }

  std::map<std::string, std::vector<Expr>> wholes;
  std::map<std::string, Expr> dims;
  std::map<std::string, std::string> first_seen;  // var -> where it was bound
  auto bindDim = [&](const std::string& var, const Expr& e, const std::string& where) {
    auto it = dims.find(var);
    if (it == dims.end()) {
      dims[var] = e;
      first_seen[var] = where;
    } else if (obs) {
      obs->push_back({it->second, e, "dim " + var + ": " + first_seen[var] + " vs " + where});
    }
  };
  const char* const kNames[] = {"lhs", "rhs", "operand 2", "operand 3"};
  for (size_t i = 0; i < r.ins.size(); ++i) {
    const Pattern& p = r.ins[i];
    const std::string who = i < 4 ? kNames[i] : "operand " + std::to_string(i);
    const std::string opname = r.ins.size() == 1 ? "operand" : who;
    if (p.whole) {
      auto it = wholes.find(p.var);
      if (it == wholes.end()) {
        wholes[p.var] = ins[i]->dims;
        first_seen[p.var] = opname;
        continue;
      }
      if (it->second.size() != ins[i]->dims.size()) {
        if (issues) {
          issues->push_back({true, n.id, n.kind,
                             "rank mismatch: " + first_seen[p.var] + " has rank " +
                                 std::to_string(it->second.size()) + ", " + opname +
                                 " has rank " + std::to_string(ins[i]->dims.size())});
        }
        return nullptr;
      }
      for (size_t d = 0; d < it->second.size() && obs; ++d) {
        obs->push_back({it->second[d], ins[i]->dims[d],
                        "dim " + std::to_string(d) + ": " + first_seen[p.var] + " vs " + opname});
      }
      continue;
    }
    if (p.dims.size() != ins[i]->dims.size()) {
      if (issues) {
        issues->push_back({true, n.id, n.kind,
                           opname + " has rank " + std::to_string(ins[i]->dims.size()) +
                               ", expected " + std::to_string(p.dims.size())});
      }
      return nullptr;
    }
    for (size_t d = 0; d < p.dims.size(); ++d) {
      const std::string where = opname + "[" + std::to_string(d) + "]";
      const std::string& pd = p.dims[d];
      if (isUpper(pd)) {
        bindDim(pd, ins[i]->dims[d], where);
      } else if (obs) {
        std::string err;
        const Expr lit = symbolic::parseExpr(pd, &err);
        if (lit) obs->push_back({ins[i]->dims[d], lit, where + " must be " + pd});
      }
    }
  }

  auto out = std::make_shared<TypeTerm>();
  out->shaped = true;
  out->name = ins[0]->name;
  out->elem = ins[0]->elem;
  if (!r.out_elem.empty()) out->elem = r.out_elem;
  if (const AttrValue* to = n.findAttr("to")) {  // a conversion names its element type
    if (to->kind == AttrValue::Kind::Ident) out->elem = to->s;
    if (to->kind == AttrValue::Kind::Type && to->type) out->elem = to->type->shaped ? to->type->elem : to->type->name;
  }
  if (r.out.whole) {
    auto it = wholes.find(r.out.var);
    if (it == wholes.end()) return nullptr;
    out->dims = it->second;
  } else {
    for (size_t d = 0; d < r.out.dims.size(); ++d) {
      const std::string& pd = r.out.dims[d];
      if (pd == "?") {
        out->dims.push_back(ExprTerm::var(dimAtom({n.id, 0}, d)));
      } else if (isUpper(pd)) {
        auto it = dims.find(pd);
        if (it == dims.end()) return nullptr;
        out->dims.push_back(it->second);
      } else {
        std::string err;
        const Expr lit = symbolic::parseExpr(pd, &err);
        if (!lit) return nullptr;
        out->dims.push_back(lit);
      }
    }
  }
  return out;
}

void Sema::compareTypes(const Node& n, const Type& want, const Type& got,
                        const std::string& what, std::vector<Obligation>* obs,
                        std::vector<verify::Issue>* issues) {
  if (!want || !got || !want->shaped || !got->shaped) return;
  if (want->dims.size() != got->dims.size()) {
    issues->push_back({true, n.id, n.kind,
                       what + ": rank " + std::to_string(got->dims.size()) + " where rank " +
                           std::to_string(want->dims.size()) + " is required"});
    return;
  }
  if (want->elem != got->elem) {
    issues->push_back({true, n.id, n.kind,
                       what + ": element type " + got->elem + " where " + want->elem +
                           " is required"});
  }
  for (size_t d = 0; d < want->dims.size(); ++d) {
    obs->push_back({want->dims[d], got->dims[d], what + " dim " + std::to_string(d)});
  }
}

void Sema::judge(const Node& n, NodeId scope, const std::vector<Obligation>& obs,
                 std::vector<verify::Issue>* out) {
  const symbolic::Solver& s = solverFor(scope);
  for (const Obligation& o : obs) {
    const Tri t = s.isEqual(o.a, o.b);
    if (t == Tri::Yes) continue;
    const std::string eq = show(o.a) + " == " + show(o.b);
    if (t == Tri::No) {
      out->push_back({true, n.id, n.kind, o.what + ": " + eq + " is false"});
    } else {
      out->push_back({false, n.id, n.kind,
                      o.what + ": cannot prove " + eq +
                          " (add a fact, or run insert-guards for a runtime check)"});
      unproven_.push_back({n.id, o.a, o.b, o.what});
    }
  }
}

// ---- judgments --------------------------------------------------------------

void Sema::checkAccess(const Node& n, const schema::Kind& k, std::vector<verify::Issue>* out) {
  int bi = -1, ai = -1;
  for (size_t i = 0; i < k.operands.size(); ++i) {
    if (k.operands[i] == "buf") bi = static_cast<int>(i);
    if (k.operands[i] == "at") ai = static_cast<int>(i);
  }
  if (bi < 0 || ai < 0 || static_cast<size_t>(std::max(bi, ai)) >= n.operands.size()) return;
  const Operand& bop = n.operands[bi];
  if (bop.kind != Operand::Kind::Value) {
    out->push_back({true, n.id, n.kind, "the buffer operand must be a value"});
    return;
  }
  const Type bt = typeOf(bop.value);
  if (!bt || !bt->shaped) {
    out->push_back({true, n.id, n.kind, "accesses a value that is not a buffer"});
    return;
  }
  const Operand& at = n.operands[ai];
  std::vector<Expr> idx;
  if (at.kind == Operand::Kind::Value) idx.push_back(ExprTerm::var(valueAtom(at.value)));
  else idx = at.exprs;
  if (idx.size() != bt->dims.size()) {
    out->push_back({true, n.id, n.kind,
                    std::to_string(idx.size()) + " index(es) for a rank-" +
                        std::to_string(bt->dims.size()) + " buffer"});
    return;
  }
  // The element type: a load's result, a store's value.
  if (n.numResults() == 1) {
    const Type t = n.typeOf(0);
    if (!t) {
      out->push_back({true, n.id, n.kind, "needs a result type (the element type " + bt->elem + ")"});
    } else if (t->shaped || printType(t) != bt->elem) {
      out->push_back({true, n.id, n.kind,
                      "result type " + printType(t) + " is not the element type " + bt->elem});
    }
  }
  const symbolic::Solver& s = solverFor(n.scope);
  const Expr zero = ExprTerm::constant(0);
  for (size_t d = 0; d < idx.size(); ++d) {
    const std::string what = "index " + std::to_string(d) + " (" + show(idx[d]) + ")";
    const Expr last = ExprTerm::add({bt->dims[d], ExprTerm::constant(-1)});
    const Tri lo = s.lessOrEqual(zero, idx[d]);
    if (lo == Tri::No) {
      out->push_back({true, n.id, n.kind, what + " is negative"});
    } else if (lo == Tri::Unknown) {
      out->push_back({false, n.id, n.kind,
                      what + ": cannot prove it is >= 0 (add a fact, or run insert-guards)"});
      unproven_.push_back({n.id, zero, idx[d], what + " >= 0", "le"});
    }
    const Tri hi = s.lessOrEqual(idx[d], last);
    if (hi == Tri::No) {
      out->push_back({true, n.id, n.kind,
                      what + " is out of bounds: it can reach " + show(bt->dims[d]) + " or more"});
    } else if (hi == Tri::Unknown) {
      out->push_back({false, n.id, n.kind,
                      what + ": cannot prove it is < " + show(bt->dims[d]) +
                          " (add a fact, or run insert-guards)"});
      unproven_.push_back({n.id, idx[d], last, what + " < " + show(bt->dims[d]), "le"});
    }
  }
}

std::vector<verify::Issue> Sema::check() {
  std::vector<verify::Issue> out;
  for (const Node& n : m_.nodes) {
    if (schema::isStructural(n.kind)) continue;
    const schema::Kind* k = reg_.find(n.kind);
    if (!k) continue;
    std::vector<Obligation> obs;

    // Shape rule: the operands must agree, and an annotation must match.
    if (!k->shape.empty()) {
      const Type inferred = applyRule(n, &obs, &out);
      const Type annotated = freshen(n.typeOf(0), {n.id, 0});
      if (inferred && annotated) {
        compareTypes(n, inferred, annotated, "result type", &obs, &out);
      }
    }

    // same_numel: the written result type must hold as many elements.
    if (k->hasTrait("same_numel") && !n.operands.empty() &&
        n.operands[0].kind == Operand::Kind::Value) {
      const Type src = typeOf(n.operands[0].value);
      const Type dst = freshen(n.typeOf(0), {n.id, 0});
      if (!dst) {
        out.push_back({true, n.id, n.kind, "needs a result type (: tensor<[...], T>)"});
      } else if (src && src->shaped && dst->shaped) {
        obs.push_back({product(src->dims), product(dst->dims), "element count"});
        if (src->elem != dst->elem) {
          out.push_back({true, n.id, n.kind,
                         "element type changes from " + src->elem + " to " + dst->elem});
        }
      }
    }

    // Every dimension an op produces must be non-negative (arguments' and fresh
    // dimensions are assumed: see build()).
    for (uint32_t p = 0; p < n.numResults(); ++p) {
      const ValueRef v{n.id, p};
      const Type t = typeOf(v);
      if (!t || !t->shaped) continue;
      const symbolic::Solver& s = solverFor(n.scope);
      const Expr zero = ExprTerm::constant(0);
      for (size_t k = 0; k < t->dims.size(); ++k) {
        const Expr& d = t->dims[k];
        if (d->kind == ExprTerm::Kind::Var && d->name == dimAtom(v, k)) continue;
        const Tri r = s.lessOrEqual(zero, d);
        if (r == Tri::Yes) continue;
        const std::string what = "dim " + std::to_string(k) + " of " +
                                 show(ExprTerm::var(valueAtom(v)));
        if (r == Tri::No) {
          out.push_back({true, n.id, n.kind, what + " is negative: " + show(d) + " < 0"});
        } else {
          out.push_back({false, n.id, n.kind,
                         what + ": cannot prove " + show(d) +
                             " >= 0 (add a fact, or run insert-guards for a runtime check)"});
          unproven_.push_back({n.id, zero, d, what + " >= 0", "le"});
        }
      }
    }

    // A memory access (`traits: access`): one index per dimension of the buffer,
    // each proven in bounds under the facts in scope. Unproven bounds go to
    // insert-guards like any other unproven equation.
    if (k->hasTrait("access")) checkAccess(n, *k, &out);

    // `dim`: the index must exist.
    if (k->expr.rfind("dim(", 0) == 0 && !n.operands.empty() &&
        n.operands[0].kind == Operand::Kind::Value) {
      const Type st = typeOf(n.operands[0].value);
      const AttrValue* idx = n.findAttr("index");
      if (st && st->shaped && idx && idx->kind == AttrValue::Kind::Int &&
          (idx->i < 0 || static_cast<size_t>(idx->i) >= st->dims.size())) {
        out.push_back({true, n.id, n.kind,
                       "index " + std::to_string(idx->i) + " is out of range for rank " +
                           std::to_string(st->dims.size())});
      }
    }

    // A return must match the function's declared types.
    if (n.kind == "return") {
      const NodeId f = m_.namingScope(n.scope);
      const Node* fn = m_.find(f);
      if (fn && fn->kind == "func" && fn->types.size() == n.operands.size()) {
        for (size_t i = 0; i < n.operands.size(); ++i) {
          if (n.operands[i].kind != Operand::Kind::Value) continue;
          compareTypes(n, freshen(fn->types[i], {fn->id, static_cast<uint32_t>(i)}),
                       typeOf(n.operands[i].value),
                       "return value " + std::to_string(i), &obs, &out);
        }
      }
    }

    // A successor argument must match its block argument's type.
    for (const Successor& s : n.successors) {
      std::vector<const Node*> bargs;
      for (const NodeId c : m_.childrenOf(s.block)) {
        const Node* a = m_.find(c);
        if (a && a->kind == "arg") bargs.push_back(a);
      }
      const Node* b = m_.find(s.block);
      for (size_t i = 0; i < s.args.size() && i < bargs.size(); ++i) {
        compareTypes(n, freshen(bargs[i]->typeOf(0), {bargs[i]->id, 0}), typeOf(s.args[i]),
                     "argument " + std::to_string(i) + " to ^" + (b ? b->symbol : "?"), &obs,
                     &out);
      }
    }

    judge(n, n.scope, obs, &out);
  }
  checkFacts(&out);
  std::set<std::string> seen;  // a fact conflict shows up once per scope that sees it
  for (const verify::Issue& is : fact_issues_) {
    if (seen.insert(is.message).second) out.push_back(is);
  }
  return out;
}

// ---- printing -----------------------------------------------------------------

std::string Sema::show(const Expr& e) {
  return symbolic::printExpr(e, [&](const std::string& a) -> std::string {
    ValueRef v;
    if (parseValueAtom(a, &v)) {
      const Node* n = m_.find(v.id);
      if (n && v.port < n->names.size() && !n->names[v.port].empty()) {
        return "%" + n->names[v.port];
      }
    }
    return a;
  });
}

std::vector<verify::Issue> verifyModule(const Module& m, const schema::Registry& reg) {
  std::vector<verify::Issue> out;
  std::string ferr;
  if (!symbolic::validateAllFacts(m, &ferr)) out.push_back({true, kInvalidId, "fact", ferr});
  std::vector<verify::Issue> structural = verify::checkModule(m, reg);
  const bool broken = verify::hasErrors(structural);
  out.insert(out.end(), structural.begin(), structural.end());
  if (!broken) {
    Sema sema(m, reg);
    std::vector<verify::Issue> semantic = sema.check();
    out.insert(out.end(), semantic.begin(), semantic.end());
  }
  return out;
}

}  // namespace brrr::sema
