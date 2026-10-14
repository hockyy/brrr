#include "ir/ir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "symbolic/expr_io.h"

namespace brrr {
namespace {

bool allDigits(const std::string& s, size_t from) {
  if (from >= s.size()) return false;
  for (size_t i = from; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

// Rename every `%<id>[#k]` atom in canonical fact text for which `f` returns a
// replacement. Facts are short; a token scan is enough.
std::string mapFactAtoms(const std::string& text,
                         const std::function<std::string(const std::string&)>& f) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    if (text[i] != '%') {
      out += text[i++];
      continue;
    }
    size_t j = i + 1;
    while (j < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_' ||
            text[j] == '.' || text[j] == '#')) {
      ++j;
    }
    out += f(text.substr(i, j - i));
    i = j;
  }
  return out;
}

std::string formatFloat(double v) {
  // Shortest form that reads back to the same double.
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  std::string s(buf);
  if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // stays a float
  return s;
}

}  // namespace

bool parseNodeIdToken(const std::string& tok, NodeId* out) {
  if (tok.size() < 2 || tok[0] != 'n' || !allDigits(tok, 1)) return false;
  *out = std::strtoull(tok.c_str() + 1, nullptr, 10);
  return *out != kInvalidId;
}

bool isValidName(const std::string& name) {
  if (name.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
    return false;
  }
  for (const char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

std::string valueAtom(const ValueRef& v) {
  std::string s = "%" + std::to_string(v.id);
  if (v.port != 0) s += "#" + std::to_string(v.port);
  return s;
}

bool parseValueAtom(const std::string& atom, ValueRef* out) {
  if (atom.size() < 2 || atom[0] != '%') return false;
  const size_t hash = atom.find('#');
  const std::string id_part = atom.substr(1, hash == std::string::npos
                                                 ? std::string::npos
                                                 : hash - 1);
  if (!allDigits(id_part, 0)) return false;
  out->id = std::strtoull(id_part.c_str(), nullptr, 10);
  out->port = 0;
  if (hash != std::string::npos) {
    const std::string p = atom.substr(hash + 1);
    if (!allDigits(p, 0)) return false;
    out->port = static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 10));
  }
  return out->id != kInvalidId;
}

std::string dimAtom(const ValueRef& v, size_t k) {
  return "?" + valueAtom(v) + "." + std::to_string(k);
}

bool atomValue(const std::string& atom, ValueRef* out) {
  std::string inner;
  size_t k = 0;
  if (symbolic::splitDimAtom(atom, &inner, &k)) return parseValueAtom(inner, out);
  return parseValueAtom(atom, out);
}

std::string printType(const Type& t, const AtomNamer& rename) {
  if (!t) return "?";
  if (!t->shaped) return t->name;
  std::string s = t->name + "<[";
  for (size_t i = 0; i < t->dims.size(); ++i) {
    if (i) s += ", ";
    s += symbolic::printExpr(t->dims[i], rename);
  }
  return s + "], " + t->elem + ">";
}

bool typesEqualText(const Type& a, const Type& b) {
  if (!a || !b) return !a && !b;
  return printType(a) == printType(b);
}

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

AttrValue AttrValue::fromBool(bool v) {
  AttrValue a;
  a.kind = Kind::Bool;
  a.b = v;
  return a;
}

AttrValue AttrValue::fromStr(std::string v) {
  AttrValue a;
  a.kind = Kind::Str;
  a.s = std::move(v);
  return a;
}

AttrValue AttrValue::fromIdent(std::string v) {
  AttrValue a;
  a.kind = Kind::Ident;
  a.s = std::move(v);
  return a;
}

std::string AttrValue::toString(const AtomNamer& rename) const {
  std::string body;
  switch (kind) {
    case Kind::Int: body = std::to_string(i); break;
    case Kind::Float: body = formatFloat(f); break;
    case Kind::Bool: body = b ? "true" : "false"; break;
    case Kind::Ident: body = s; break;
    case Kind::Type: body = printType(type, rename); break;
    case Kind::Str: {
      body = "\"";
      for (const char c : s) {
        if (c == '\\' || c == '"') body += '\\';
        body += c;
      }
      body += '"';
      break;
    }
    case Kind::Array: {
      body = "[";
      for (size_t k = 0; k < items.size(); ++k) {
        if (k) body += ", ";
        body += items[k].toString(rename);
      }
      body += "]";
      break;
    }
  }
  if (ascribed) return "(" + body + " : " + printType(ascribed, rename) + ")";
  return body;
}

Operand Operand::ofValue(ValueRef v) {
  Operand o;
  o.kind = Kind::Value;
  o.value = v;
  return o;
}

Operand Operand::ofExpr(symbolic::Expr e) {
  Operand o;
  o.kind = Kind::Expr;
  o.exprs.push_back(std::move(e));
  return o;
}

Operand Operand::ofList(std::vector<symbolic::Expr> es) {
  Operand o;
  o.kind = Kind::List;
  o.exprs = std::move(es);
  return o;
}

void Operand::uses(std::vector<ValueRef>* out) const {
  if (kind == Kind::Value) {
    out->push_back(value);
    return;
  }
  std::set<std::string> atoms;
  for (const auto& e : exprs) symbolic::collectVars(e, &atoms);
  for (const std::string& a : atoms) {
    ValueRef v;
    if (atomValue(a, &v)) out->push_back(v);
  }
}

const AttrValue* Node::findAttr(const std::string& key) const {
  auto it = attrs.find(key);
  return it == attrs.end() ? nullptr : &it->second;
}

std::vector<ValueRef> Node::uses() const {
  std::vector<ValueRef> out;
  for (const Operand& o : operands) o.uses(&out);
  for (const Successor& s : successors) {
    out.insert(out.end(), s.args.begin(), s.args.end());
  }
  return out;
}

NodeId Module::allocId() { return next_id_++; }

void Module::reserveId(NodeId id) {
  if (id >= next_id_) next_id_ = id + 1;
}

Node* Module::find(NodeId id) {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : &nodes[it->second];
}

const Node* Module::find(NodeId id) const {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : &nodes[it->second];
}

void Module::reindex() {
  index_.clear();
  for (size_t i = 0; i < nodes.size(); ++i) index_[nodes[i].id] = i;
}

Node& Module::append(Node n) {
  if (n.id == kInvalidId) n.id = allocId();
  reserveId(n.id);
  if (n.scope == kInvalidId) top_level.push_back(n.id);
  index_[n.id] = nodes.size();
  nodes.push_back(std::move(n));
  return nodes.back();
}

Node& Module::insertAfter(NodeId after, Node n) {
  if (n.id == kInvalidId) n.id = allocId();
  reserveId(n.id);
  auto it = std::find_if(nodes.begin(), nodes.end(),
                         [&](const Node& x) { return x.id == after; });
  const size_t pos = it == nodes.end() ? nodes.size() : (it - nodes.begin()) + 1;
  const NodeId id = n.id;
  if (n.scope == kInvalidId) {
    auto t = std::find(top_level.begin(), top_level.end(), after);
    top_level.insert(t == top_level.end() ? t : t + 1, id);
  }
  nodes.insert(nodes.begin() + static_cast<long>(pos), std::move(n));
  reindex();
  return *find(id);
}

Node& Module::insertBefore(NodeId before, Node n) {
  if (n.id == kInvalidId) n.id = allocId();
  reserveId(n.id);
  auto it = std::find_if(nodes.begin(), nodes.end(),
                         [&](const Node& x) { return x.id == before; });
  const NodeId id = n.id;
  if (n.scope == kInvalidId) {
    top_level.insert(std::find(top_level.begin(), top_level.end(), before), id);
  }
  nodes.insert(it, std::move(n));
  reindex();
  return *find(id);
}

std::vector<NodeId> Module::childrenOf(NodeId scope) const {
  std::vector<NodeId> out;
  for (const auto& n : nodes) {
    if (n.scope == scope) out.push_back(n.id);
  }
  return out;
}

void Module::replaceAllUses(ValueRef from, ValueRef to) {
  const std::string fa = valueAtom(from);
  const std::string ta = valueAtom(to);
  auto ren = [&](const std::string& a) { return a == fa ? ta : a; };
  for (Node& n : nodes) {
    bool changed = false;
    for (Operand& o : n.operands) {
      if (o.kind == Operand::Kind::Value) {
        if (o.value == from) {
          o.value = to;
          changed = true;
        }
        continue;
      }
      for (auto& e : o.exprs) {
        auto e2 = symbolic::mapVars(e, ren);
        if (!symbolic::structurallyEqual(e, e2)) {
          e = e2;
          changed = true;
        }
      }
    }
    for (Successor& s : n.successors) {
      for (ValueRef& a : s.args) {
        if (a == from) {
          a = to;
          changed = true;
        }
      }
    }
    for (Type& t : n.types) {
      if (!t || !t->shaped) continue;
      auto t2 = std::make_shared<TypeTerm>(*t);
      bool tchanged = false;
      for (auto& d : t2->dims) {
        auto d2 = symbolic::mapVars(d, ren);
        if (!symbolic::structurallyEqual(d, d2)) {
          d = d2;
          tchanged = true;
        }
      }
      if (tchanged) {
        t = t2;
        changed = true;
      }
    }
    if (changed) log("replace", n.id, fa + " -> " + ta);
  }
  for (FactEntry& f : facts) f.text = mapFactAtoms(f.text, ren);
}

void Module::erase(NodeId id) {
  std::set<NodeId> doomed{id};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const Node& n : nodes) {
      if (!doomed.count(n.id) && doomed.count(n.scope)) {
        doomed.insert(n.id);
        grew = true;
      }
    }
  }
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                             [&](const Node& n) { return doomed.count(n.id) > 0; }),
              nodes.end());
  top_level.erase(std::remove_if(top_level.begin(), top_level.end(),
                                 [&](NodeId x) { return doomed.count(x) > 0; }),
                  top_level.end());
  // A fact about an erased value (or inside an erased scope) goes with it.
  facts.erase(std::remove_if(facts.begin(), facts.end(),
                             [&](const FactEntry& f) {
                               if (doomed.count(f.scope)) return true;
                               bool dangling = false;
                               mapFactAtoms(f.text, [&](const std::string& a) {
                                 ValueRef v;
                                 if (parseValueAtom(a, &v) && doomed.count(v.id)) {
                                   dangling = true;
                                 }
                                 return a;
                               });
                               return dangling;
                             }),
              facts.end());
  reindex();
  log("erase", id, std::to_string(doomed.size()) + " node(s)");
}

void Module::setType(ValueRef v, Type t) {
  Node* n = find(v.id);
  if (!n) return;
  if (n->types.size() < n->numResults()) n->types.resize(n->numResults());
  if (v.port >= n->types.size()) return;
  n->types[v.port] = t;
  log("set-type", v.id, valueAtom(v) + " : " + printType(t));
}

bool Module::rename(ValueRef v, const std::string& new_name, std::string* err) {
  Node* n = find(v.id);
  if (!n || v.port >= n->numResults()) {
    if (err) *err = "no such value " + valueAtom(v);
    return false;
  }
  if (!new_name.empty()) {
    if (!isValidName(new_name)) {
      if (err) *err = "invalid name '" + new_name + "'";
      return false;
    }
    ValueRef other;
    const NodeId ns = namingScope(n->scope);
    if (resolveName(n->scope, new_name, &other) && !(other == v) &&
        namingScope(find(other.id)->scope) == ns) {
      if (err) *err = "name '%" + new_name + "' already used in this function";
      return false;
    }
  }
  const std::string old = n->names[v.port];
  n->names[v.port] = new_name;
  log("rename", v.id,
      (old.empty() ? valueAtom(v) : "%" + old) + " -> " +
          (new_name.empty() ? valueAtom(v) : "%" + new_name));
  return true;
}

size_t Module::useCount(ValueRef v) const {
  size_t count = 0;
  for (const Node& n : nodes) {
    for (const ValueRef& u : n.uses()) count += (u == v);
    for (const Type& t : n.types) {
      if (!t || !t->shaped) continue;
      for (const auto& d : t->dims) {
        std::set<std::string> atoms;
        symbolic::collectVars(d, &atoms);
        for (const std::string& a : atoms) {
          ValueRef u;
          count += atomValue(a, &u) && u == v;
        }
      }
    }
  }
  return count;
}

void Module::log(std::string action, NodeId id, std::string detail) {
  journal.push_back({std::move(action), id, std::move(detail)});
}

NodeId Module::namingScope(NodeId scope) const {
  for (NodeId s = scope; s != kInvalidId;) {
    const Node* n = find(s);
    if (!n) return kInvalidId;
    if (n->kind == "func" || n->kind == "module") return s;
    s = n->scope;
  }
  return kInvalidId;
}

bool Module::resolveName(NodeId scope, const std::string& name,
                         ValueRef* out) const {
  for (NodeId ns = namingScope(scope);;) {
    for (const Node& n : nodes) {
      if (namingScope(n.scope) != ns) continue;
      for (uint32_t p = 0; p < n.names.size(); ++p) {
        if (n.names[p] == name) {
          *out = {n.id, p};
          return true;
        }
      }
    }
    if (ns == kInvalidId) return false;
    const Node* nn = find(ns);
    ns = nn ? namingScope(nn->scope) : kInvalidId;
  }
}

}  // namespace brrr
