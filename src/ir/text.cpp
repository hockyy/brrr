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

bool parseNodeIdToken(const std::string& tok, NodeId* out) {
  if (tok.size() < 2 || tok[0] != 'n') return false;
  for (size_t i = 1; i < tok.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return false;
  }
  *out = std::strtoull(tok.c_str() + 1, nullptr, 10);
  return *out != kInvalidId;
}

bool parseValueRef(const std::string& tok, ValueRef* out) {
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
  auto printNodeLine = [&](const Node& n, const std::string& indent) {
    out += indent + "n" + std::to_string(n.id) + " = " + n.kind;
    for (const ValueRef& vr : n.operands) {
      out += " n" + std::to_string(vr.id);
      if (vr.port != 0) out += "." + std::to_string(vr.port);
    }
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
  ParseResult res;
  res.ok = true;
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  std::vector<NodeId> scope_stack;
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

    // Node line: nID = kind [ref...] [key=value...]
    if (tok.size() < 3 || tok[1] != "=") { fail("expected node assignment"); break; }
    NodeId id;
    if (!parseNodeIdToken(tok[0], &id)) { fail("malformed node id"); break; }
    if (res.module.contains(id)) { fail("duplicate node id"); break; }
    Node n;
    n.id = id;
    n.kind = tok[2];
    n.scope = scope_stack.empty() ? kInvalidId : scope_stack.back();
    bool bad = false;
    for (size_t i = 3; i < tok.size(); ++i) {
      const std::string& t = tok[i];
      ValueRef vr;
      if (!t.empty() && t[0] == 'n' && parseValueRef(t, &vr)) {
        n.operands.push_back(vr);
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
  }

  if (res.ok && !scope_stack.empty()) fail("unterminated function block");

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
