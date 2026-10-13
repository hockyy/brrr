#include "ir/text.h"

#include <cctype>
#include <cstdlib>
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

}  // namespace

std::string printModule(const Module& m) {
  std::string out = "# bridgeir v0\n";

  // Facts are part of the document and round-trip verbatim.
  for (const std::string& f : m.facts) out += f + "\n";

  // Resolve a name from a scope: scope first, then module level.
  auto resolveName = [&](NodeId from_scope, const std::string& nm) -> const Node* {
    const Node* global = nullptr;
    for (const Node& n : m.nodes) {
      if (n.name != nm) continue;
      if (n.scope == from_scope) return &n;
      if (n.scope == kInvalidId) global = &n;
    }
    return global;
  };

  // Prefer the target's name when it resolves back to the same node; otherwise id.
  auto refString = [&](const Node& from, const ValueRef& vr) {
    const Node* t = m.find(vr.id);
    std::string ref;
    if (t && !t->name.empty()) {
      const Node* r = resolveName(from.scope, t->name);
      if (r && r->id == vr.id) ref = t->name;
    }
    if (ref.empty()) ref = "n" + std::to_string(vr.id);
    if (vr.port != 0) ref += "." + std::to_string(vr.port);
    return ref;
  };

  auto printNodeLine = [&](const Node& n, const std::string& indent) {
    out += indent + "n" + std::to_string(n.id);
    if (!n.name.empty()) out += " " + n.name;
    out += " = " + n.kind;
    for (const ValueRef& vr : n.operands) out += " " + refString(n, vr);
    for (const auto& [k, v] : n.attrs) out += " " + k + "=" + v.toString();
    out += "\n";
  };

  for (NodeId id : m.top_level) {
    const Node* n = m.find(id);
    if (!n) continue;
    if (n->kind == "func") {
      out += "func @" + n->attrString("sym", "anon") + "(n" +
             std::to_string(n->id) + ") {\n";
      for (NodeId cid : m.childrenOf(n->id)) {
        const Node* c = m.find(cid);
        if (c) printNodeLine(*c, "  ");
      }
      out += "}\n";
    } else {
      printNodeLine(*n, "");
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
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  std::vector<NodeId> scope_stack;
  std::vector<Pending> pending;
  auto fail = [&](const std::string& msg) {
    res.ok = false;
    res.error = msg;
    res.line = lineno;
  };

  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();

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

    const std::vector<std::string> tok = tokenize(line);
    if (tok.empty()) continue;

    // Facts are part of the document; kept raw here (the symbolic layer parses
    // them, and ir/ does not depend on it).
    if (tok[0] == "fact") {
      const size_t b = line.find_first_not_of(" \t");
      const size_t e = line.find_last_not_of(" \t");
      res.module.facts.push_back(line.substr(b, e - b + 1));
      continue;
    }

    if (tok.size() == 1 && tok[0] == "}") {
      if (scope_stack.empty()) { fail("unmatched '}'"); break; }
      scope_stack.pop_back();
      continue;
    }

    if (tok[0] == "func") {
      if (tok.size() != 3 || tok[2] != "{") {
        fail("expected: func @name(nID) {");
        break;
      }
      const std::string& head = tok[1];
      const size_t lp = head.find('(');
      if (head.empty() || head[0] != '@' || lp == std::string::npos ||
          head.back() != ')') {
        fail("malformed function header");
        break;
      }
      const std::string sym = head.substr(1, lp - 1);
      NodeId id;
      if (!parseNodeIdToken(head.substr(lp + 1, head.size() - lp - 2), &id)) {
        fail("malformed function id");
        break;
      }
      if (res.module.contains(id)) { fail("duplicate node id"); break; }
      Node n;
      n.id = id;
      n.kind = "func";
      n.attrs["sym"] = AttrValue::fromSym(sym);
      res.module.appendParsedNode(std::move(n));
      scope_stack.push_back(id);
      continue;
    }

    // Node: nID [name] = kind [ref...] [key=value...]
    NodeId id;
    if (!parseNodeIdToken(tok[0], &id)) { fail("malformed node id"); break; }
    if (res.module.contains(id)) { fail("duplicate node id"); break; }

    size_t i = 1;
    std::string name;
    if (i < tok.size() && tok[i] != "=") {
      name = tok[i];
      if (!isValidNodeName(name)) {
        fail("invalid node name '" + name + "'");
        break;
      }
      ++i;
    }
    if (i >= tok.size() || tok[i] != "=") { fail("expected '='"); break; }
    ++i;
    if (i >= tok.size()) { fail("expected node kind"); break; }

    Node n;
    n.id = id;
    n.name = name;
    n.kind = tok[i++];
    n.scope = scope_stack.empty() ? kInvalidId : scope_stack.back();

    if (!name.empty()) {
      bool dup = false;
      for (const Node& o : res.module.nodes) {
        if (o.scope == n.scope && o.name == name) { dup = true; break; }
      }
      if (dup) { fail("duplicate name '" + name + "' in scope"); break; }
    }

    bool bad = false;
    std::vector<size_t> local_pending;
    for (; i < tok.size(); ++i) {
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
  }

  if (res.ok && !scope_stack.empty()) fail("unterminated function block");

  // Resolve name references (scope first, then module level; order-independent).
  if (res.ok) {
    for (const Pending& p : pending) {
      const Node& from = res.module.nodes[p.node_index];
      const Node* found = nullptr;
      for (const Node& n : res.module.nodes) {
        if (n.name != p.name) continue;
        if (n.scope == from.scope) { found = &n; break; }
        if (n.scope == kInvalidId && !found) found = &n;
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
