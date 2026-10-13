#include "ir/text.h"

#include "ir/sugar.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace bridgeir {
namespace {

// Whitespace-separated tokens; double quotes keep spaces/escapes together.
std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_str = false;
  bool have = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_str) {
      cur += c;
      if (c == '\\' && i + 1 < line.size()) {
        cur += line[++i];
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      have = true;
      cur += c;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty() || have) {
        out.push_back(cur);
        cur.clear();
        have = false;
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty() || have) out.push_back(cur);
  return out;
}

// A scope header may carry a typed argument list: `block (int64 n9, int64 n15)`.
// Tokens are whitespace-split, so glue a parenthesized group back into one token
// (only if it actually closes on this line; otherwise leave it for the parser to
// reject).
std::vector<std::string> glueParenGroups(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i].size() > 1 && in[i][0] == '(' && in[i].back() != ')') {
      std::string merged = in[i];
      size_t j = i;
      bool closed = false;
      while (++j < in.size()) {
        merged += " " + in[j];
        if (!in[j].empty() && in[j].back() == ')') { closed = true; break; }
      }
      if (closed) {
        out.push_back(merged);
        i = j;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

// `nID[.port]` reference form.
bool parseIdRef(const std::string& tok, ValueRef* out) {
  const size_t dot = tok.find('.');
  const std::string id_part = dot == std::string::npos ? tok : tok.substr(0, dot);
  NodeId id;
  if (!parseNodeIdToken(id_part, &id)) return false;
  out->id = id;
  out->port = 0;
  if (dot != std::string::npos) {
    const std::string p = tok.substr(dot + 1);
    if (p.empty()) return false;
    for (char c : p) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    out->port = static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 10));
  }
  return true;
}

// `name[.port]` reference form.
bool parseNameRef(const std::string& tok, std::string* base, uint32_t* port) {
  const size_t dot = tok.find('.');
  *base = dot == std::string::npos ? tok : tok.substr(0, dot);
  *port = 0;
  if (!isValidNodeName(*base)) return false;
  if (dot != std::string::npos) {
    const std::string p = tok.substr(dot + 1);
    if (p.empty()) return false;
    for (char c : p) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    *port = static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 10));
  }
  return true;
}

bool parseInt(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  const size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (start >= s.size()) return false;
  for (size_t i = start; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  *out = std::strtoll(s.c_str(), nullptr, 10);
  return true;
}

bool parseFloat(const std::string& s, double* out) {
  if (s.find_first_of(".eE") == std::string::npos) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return false;
  *out = v;
  return true;
}

bool parseAttrValue(const std::string& tok, AttrValue* out) {
  if (tok.empty()) return false;
  if (tok[0] == '@') {
    *out = AttrValue::fromSym(tok.substr(1));
    return true;
  }
  if (tok[0] == '"') {
    if (tok.size() < 2 || tok.back() != '"') return false;
    std::string v;
    for (size_t i = 1; i + 1 < tok.size(); ++i) {
      char c = tok[i];
      if (c == '\\' && i + 2 < tok.size()) {
        v += tok[++i];
      } else {
        v += c;
      }
    }
    *out = AttrValue::fromStr(std::move(v));
    return true;
  }
  int64_t iv;
  if (parseInt(tok, &iv)) {
    *out = AttrValue::fromInt(iv);
    return true;
  }
  double dv;
  if (parseFloat(tok, &dv)) {
    *out = AttrValue::fromFloat(dv);
    return true;
  }
  return false;
}

// Resolve a name from a scope chain: innermost scope outward, then root.
const Node* resolveName(const Module& m, NodeId from_scope,
                        const std::string& nm) {
  NodeId scope = from_scope;
  while (scope != kInvalidId) {
    for (const Node& n : m.nodes) {
      if (n.scope == scope && n.name == nm) return &n;
    }
    const Node* s = m.find(scope);
    scope = s ? s->scope : kInvalidId;
  }
  for (const Node& n : m.nodes) {
    if (n.scope == kInvalidId && n.name == nm) return &n;
  }
  return nullptr;
}

// Prefer the target's name when it resolves back to the same node; otherwise id.
std::string refText(const Module& m, NodeId from_scope, const ValueRef& vr) {
  const Node* t = m.find(vr.id);
  std::string ref;
  // Arguments are ids: a `param`'s name is never an alias, never a reference.
  if (t && t->kind != "param" && !t->name.empty()) {
    const Node* r = resolveName(m, from_scope, t->name);
    if (r && r->id == vr.id) ref = t->name;
  }
  if (ref.empty()) ref = "n" + std::to_string(vr.id);
  if (vr.port != 0) ref += "." + std::to_string(vr.port);
  return ref;
}

std::string nodeLine(const Module& m, const Node& n, const std::string& indent) {
  std::string line = indent + "n" + std::to_string(n.id);
  if (!n.name.empty() && n.kind != "param") line += " " + n.name;
  line += " = " + n.kind;
  for (const ValueRef& vr : n.operands) line += " " + refText(m, n.scope, vr);
  for (const auto& [k, v] : n.attrs) line += " " + k + "=" + v.toString();
  return line + "\n";
}

// Any node opened with `{` is a scope: modules, funcs, blocks, loops. Scope facts
// (invariants) print inside the body they were written in.
std::string scopeBlock(const Module& m, const Node& n,
                       const std::string& indent) {
  const bool named_scope = (n.kind != "func" && n.kind != "module");
  std::string out;
  if (!named_scope) {
    out = indent + n.kind + " @" + n.attrString("sym", "anon") + "(n" +
          std::to_string(n.id) + ")";
  } else {
    out = indent + "n" + std::to_string(n.id);
    if (!n.name.empty()) out += " " + n.name;
    out += " = " + n.kind;
  }
  // Leading `param` children are the scope's arguments: print them as the typed
  // header list `(int64 n9 acc, int64 n15)` and skip them in the body, so an
  // argument is declared once rather than as a `param idx=k` line.
  std::vector<NodeId> header_args;
  if (named_scope) {
    for (const NodeId cid : m.childrenOf(n.id)) {
      const Node* c = m.find(cid);
      if (!c || c->kind != "param") break;  // only the leading run
      header_args.push_back(cid);
    }
    if (!header_args.empty()) {
      out += " (";
      for (size_t k = 0; k < header_args.size(); ++k) {
        if (k) out += ", ";
        const Node* a = m.find(header_args[k]);
        if (const AttrValue* t = a->findAttr("type")) out += t->s + " ";
        out += "n" + std::to_string(a->id);   // args are ids; no name alias
      }
      out += ")";
    }
  }
  if (named_scope) {
    for (const ValueRef& vr : n.operands) out += " " + refText(m, n.scope, vr);
    for (const auto& [k, v] : n.attrs) out += " " + k + "=" + v.toString();
  }
  out += " {\n";
  for (const Module::FactEntry& f : m.facts) {
    if (f.scope == n.id) out += indent + "  " + f.line + "\n";
  }
  for (const NodeId cid : m.childrenOf(n.id)) {
    if (std::find(header_args.begin(), header_args.end(), cid) !=
        header_args.end()) {
      continue;  // already printed in the header
    }
    const Node* c = m.find(cid);
    if (!c) continue;
    out += c->has_body ? scopeBlock(m, *c, indent + "  ")
                       : nodeLine(m, *c, indent + "  ");
  }
  out += indent + "}\n";
  return out;
}

}  // namespace

std::string printModule(const Module& m) {
  std::string out = "# bridgeir v0\n";

  // Root facts are part of the document and round-trip verbatim.
  for (const Module::FactEntry& f : m.facts) {
    if (f.scope == kInvalidId) out += f.line + "\n";
  }

  for (const NodeId id : m.top_level) {
    const Node* n = m.find(id);
    if (!n) continue;
    if (n->has_body) {
      out += scopeBlock(m, *n, "");
    } else {
      out += nodeLine(m, *n, "");
    }
  }
  return out;
}

ParseResult parseModule(const std::string& text) {
  struct Pending {
    size_t node_index = 0;
    size_t operand_index = 0;
    std::string name;
    int line = 0;
  };

  ParseResult res;
  res.ok = true;

  // Collect lines first: a global id reservation keeps synthesized loop nodes
  // from colliding with explicit ids, and `while` needs lookahead for its
  // carried names.
  std::vector<std::string> lines;
  {
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  // C++-shaped sugar (`for`/`while`/`if`) lowers to canonical lines first.
  {
    std::string serr;
    if (!lowerCppSugar(&lines, &serr)) {
      res.ok = false;
      res.error = serr;
      res.line = 0;
      return res;
    }
  }

  for (const std::string& l : lines) {
    for (const std::string& t : tokenize(l)) {
      std::string base = t;
      const size_t dot = base.find('.');
      if (dot != std::string::npos) base = base.substr(0, dot);
      NodeId id = kInvalidId;
      if (parseNodeIdToken(base, &id)) res.module.reserveId(id);
    }
  }

  // C++-shaped loop sugar: scopes currently open as `for`/`while`. The header is
  // desugared into `loop` + `param`s + `loop.yield`; nothing new in the core.
  struct SugarLoop {
    size_t depth = 0;  // scope_stack size while the body is open
    std::vector<std::string> carried;
    std::string cond_tok;
    std::vector<std::string> next_tok;  // parallel to carried
  };
  std::vector<SugarLoop> sugar;

  std::string line;
  int lineno = 0;
  std::vector<NodeId> scope_stack;
  std::vector<Pending> pending;
  auto fail = [&](const std::string& msg) {
    res.ok = false;
    res.error = msg;
    res.line = lineno;
  };

  for (const std::string& raw_line : lines) {
    line = raw_line;
    ++lineno;

    // Strip comments (outside quotes), full-line or trailing.
    bool in_str = false;
    size_t cut = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (in_str) {
        if (c == '\\') ++i;
        else if (c == '"') in_str = false;
        continue;
      }
      if (c == '"') in_str = true;
      else if (c == '#') { cut = i; break; }
    }
    if (cut != std::string::npos) line = line.substr(0, cut);

    const std::vector<std::string> tok = glueParenGroups(tokenize(line));
    if (tok.empty()) continue;

    // Facts are scoped: they hold where they are written and are visible to
    // inner scopes through the scope chain.
    if (tok[0] == "fact") {
      const size_t b = line.find_first_not_of(" \t");
      const size_t e = line.find_last_not_of(" \t");
      res.module.facts.push_back(
          {scope_stack.empty() ? kInvalidId : scope_stack.back(),
           line.substr(b, e - b + 1)});
      continue;
    }

    // Vocabulary belongs to compiler modules, never to a document.
    if (tok[0] == "scalar" || tok[0] == "type" || tok[0] == "kind") {
      fail("vocabulary (" + tok[0] +
           " ...) cannot be declared in a document; define it in a module and "
           "`import` it");
      break;
    }

    // Assignment inside a sugar loop body: `name = value` (the C++-shaped update).
    {
      NodeId maybe_id = kInvalidId;
      const bool is_id = parseNodeIdToken(tok[0], &maybe_id);
      if (!is_id && tok.size() >= 2 && tok[1] == "=") {
        if (sugar.empty() || sugar.back().depth != scope_stack.size()) {
          fail("assignment outside a for/while loop");
          break;
        }
        if (tok.size() != 3) {
          fail("assignment must be 'name = value'");
          break;
        }
        size_t slot = sugar.back().carried.size();
        for (size_t k = 0; k < sugar.back().carried.size(); ++k) {
          if (sugar.back().carried[k] == tok[0]) slot = k;
        }
        if (slot == sugar.back().carried.size()) {
          fail("assignment to '" + tok[0] + "', which the loop does not carry");
          break;
        }
        sugar.back().next_tok[slot] = tok[2];
        continue;
      }
    }

    if (tok.size() == 1 && tok[0] == "}") {
      if (scope_stack.empty()) { fail("unmatched '}'"); break; }
      scope_stack.pop_back();
      continue;
    }

    if (tok[0] == "func" || tok[0] == "module") {
      const std::string what = tok[0];
      if (tok.size() != 3 || tok[2] != "{") {
        fail("expected: " + what + " @name(nID) {");
        break;
      }
      const std::string& head = tok[1];
      const size_t lp = head.find('(');
      if (head.empty() || head[0] != '@' || lp == std::string::npos ||
          head.back() != ')') {
        fail("malformed " + what + " header");
        break;
      }
      const std::string sym = head.substr(1, lp - 1);
      NodeId id;
      if (!parseNodeIdToken(head.substr(lp + 1, head.size() - lp - 2), &id)) {
        fail("malformed " + what + " id");
        break;
      }
      if (res.module.contains(id)) { fail("duplicate node id"); break; }
      Node n;
      n.id = id;
      n.kind = what;
      n.scope = scope_stack.empty() ? kInvalidId : scope_stack.back();
      n.has_body = true;
      n.attrs["sym"] = AttrValue::fromSym(sym);
      res.module.appendParsedNode(std::move(n));
      scope_stack.push_back(id);
      continue;
    }

    // Node: nID [name] = kind [ref...] [key=value...] [{]
    NodeId id;
    if (!parseNodeIdToken(tok[0], &id)) { fail("malformed node id"); break; }
    if (res.module.contains(id)) { fail("duplicate node id"); break; }

    const bool opens_body = tok.back() == "{";
    const size_t end = opens_body ? tok.size() - 1 : tok.size();

    size_t i = 1;
    std::string name;
    if (i < end && tok[i] != "=") {
      name = tok[i];
      if (!isValidNodeName(name)) {
        fail("invalid node name '" + name + "'");
        break;
      }
      ++i;
    }
    if (i >= end || tok[i] != "=") { fail("expected '='"); break; }
    ++i;
    if (i >= end) { fail("expected node kind"); break; }

    Node n;
    n.id = id;
    n.name = name;
    n.kind = tok[i++];
    n.scope = scope_stack.empty() ? kInvalidId : scope_stack.back();
    n.has_body = opens_body;

    // Optional typed argument list in the scope header: `block (int64 n9, int64 n15)`
    // or `block (int64 acc)` (id auto-assigned) or `block (n9 acc)`. Arguments are
    // the body's block arguments; they are stored as `param` nodes so edges, loops
    // and the checker need no new path. `idx` is the declared position.
    struct HeaderArg {
      NodeId id = kInvalidId;
      std::string type;
    };
    bool bad = false;
    std::vector<HeaderArg> header_args;
    if (i < end && tok[i].size() > 1 && tok[i][0] == '(' &&
        tok[i].back() == ')') {
      if (!opens_body) {
        fail("an argument list requires a body");
        break;
      }
      const std::string inner = tok[i].substr(1, tok[i].size() - 2);
      ++i;
      std::vector<std::string> items;
      std::string cur;
      for (const char c : inner) {
        if (c == ',') {
          items.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      items.push_back(cur);
      for (const std::string& item : items) {
        const std::vector<std::string> its = tokenize(item);
        if (its.empty()) {
          fail("empty argument in header list");
          bad = true;
          break;
        }
        HeaderArg ha;
        size_t id_pos = its.size();
        for (size_t k = 0; k < its.size(); ++k) {
          NodeId wid = kInvalidId;
          if (parseNodeIdToken(its[k], &wid)) {
            ha.id = wid;
            id_pos = k;
            break;
          }
        }
        // Entries are `TYPE ID` (or just `ID`). There is no name alias: an
        // argument is identified by its id, and nothing else.
        if (id_pos == its.size() || id_pos + 1 != its.size()) {
          fail("argument list entries are 'TYPE ID' (ids are nN), got '" + item +
               "'");
          bad = true;
          break;
        }
        if (id_pos == 1) ha.type = its[0];
        if (res.module.contains(ha.id)) {
          fail("duplicate argument id in header list");
          bad = true;
          break;
        }
        header_args.push_back(ha);
      }
      if (bad) break;
    }

    if (!name.empty()) {
      bool dup = false;
      for (const Node& o : res.module.nodes) {
        if (o.scope == n.scope && o.name == name) { dup = true; break; }
      }
      if (dup) { fail("duplicate name '" + name + "' in scope"); break; }
    }

    std::vector<size_t> local_pending;
    for (; i < end; ++i) {
      const std::string& t = tok[i];
      ValueRef vr;
      if (parseIdRef(t, &vr)) {
        n.operands.push_back(vr);
        continue;
      }
      std::string base;
      uint32_t port = 0;
      if (parseNameRef(t, &base, &port)) {
        n.operands.push_back({kInvalidId, port});
        pending.push_back({0, n.operands.size() - 1, base, lineno});
        local_pending.push_back(pending.size() - 1);
        continue;
      }
      const size_t eq = t.find('=');
      if (eq == std::string::npos || eq == 0) {
        fail("bad token '" + t + "'");
        bad = true;
        break;
      }
      AttrValue av;
      if (!parseAttrValue(t.substr(eq + 1), &av)) {
        fail("bad attribute value in '" + t + "'");
        bad = true;
        break;
      }
      n.attrs[t.substr(0, eq)] = std::move(av);
    }
    if (bad) break;
    res.module.appendParsedNode(std::move(n));
    const size_t node_index = res.module.nodes.size() - 1;
    for (size_t pi : local_pending) pending[pi].node_index = node_index;
    // Header arguments become this scope's `param` children, in declared order.
    for (size_t k = 0; k < header_args.size(); ++k) {
      res.module.reserveId(header_args[k].id);
      Node p;
      p.id = header_args[k].id;
      p.kind = "param";
      p.scope = id;
      p.attrs["idx"] = AttrValue::fromInt(static_cast<int64_t>(k));
      if (!header_args[k].type.empty()) {
        p.attrs["type"] = AttrValue::fromStr(header_args[k].type);
      }
      res.module.appendParsedNode(std::move(p));
    }
    if (opens_body) scope_stack.push_back(id);
  }

  if (res.ok && !scope_stack.empty()) fail("unterminated block");

  // Resolve name references (scope chain first, then root; order-independent).
  if (res.ok) {
    for (const Pending& p : pending) {
      const Node& from = res.module.nodes[p.node_index];
      const Node* found = nullptr;
      NodeId scope = from.scope;
      while (scope != kInvalidId && !found) {
        for (const Node& n : res.module.nodes) {
          if (n.scope == scope && n.name == p.name) { found = &n; break; }
        }
        const Node* s = res.module.find(scope);
        scope = s ? s->scope : kInvalidId;
      }
      if (!found) {
        for (const Node& n : res.module.nodes) {
          if (n.scope == kInvalidId && n.name == p.name) { found = &n; break; }
        }
      }
      if (!found) {
        res.ok = false;
        res.line = p.line;
        res.error = "unknown name '" + p.name + "'";
        break;
      }
      res.module.nodes[p.node_index].operands[p.operand_index].id = found->id;
    }
  }

  // Structural check: operands must reference existing nodes.
  if (res.ok) {
    for (const Node& n : res.module.nodes) {
      for (const ValueRef& vr : n.operands) {
        if (!res.module.contains(vr.id)) {
          fail("dangling operand n" + std::to_string(vr.id) + " in n" +
               std::to_string(n.id));
          break;
        }
      }
      if (!res.ok) break;
    }
  }
  return res;
}

}  // namespace bridgeir
