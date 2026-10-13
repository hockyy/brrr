#include "io/c_like.h"

#include <cstdio>

namespace bridgeir::io {
namespace {

std::string attrValueText(const AttrValue& v) {
  switch (v.kind) {
    case AttrValue::Kind::Int:
      return std::to_string(v.i);
    case AttrValue::Kind::Float: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", v.f);
      std::string s(buf);
      if (s.find_first_of(".eE") == std::string::npos) s += ".0";
      return s;
    }
    case AttrValue::Kind::Str: {
      // Expression-like strings (dims, shapes, types) render bare for fluency.
      bool bare = !v.s.empty();
      for (const char c : v.s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == ' ' ||
                        c == '+' || c == '*' || c == '(' || c == ')' ||
                        c == ',' || c == '.' || c == '<' || c == '>' ||
                        c == '[' || c == ']' || c == '/' || c == '-';
        if (!ok) {
          bare = false;
          break;
        }
      }
      return bare ? v.s : ("\"" + v.s + "\"");
    }
    case AttrValue::Kind::Sym:
      return "@" + v.s;
  }
  return {};
}

std::string refText(const Module& m, const ValueRef& vr) {
  const Node* t = m.find(vr.id);
  std::string base =
      (t && !t->name.empty()) ? t->name : ("_n" + std::to_string(vr.id));
  if (vr.port != 0) base += "." + std::to_string(vr.port);
  return base;
}

std::string renderNode(const Module& m, const Node& n) {
  std::string line;

  if (n.kind == "func.return" || n.kind == "yield") {
    line = (n.kind == "func.return") ? "return" : "yield";
    for (const ValueRef& vr : n.operands) line += " " + refText(m, vr);
    line += ";";
  } else {
    // A `type` attr, when present, becomes the declaration type (C++-like).
    std::string decl = "auto";
    bool type_promoted = false;
    if (const AttrValue* t = n.findAttr("type")) {
      if (t->kind == AttrValue::Kind::Str) {
        decl = t->s;
        type_promoted = true;
      } else if (t->kind == AttrValue::Kind::Sym) {
        decl = t->s;
        type_promoted = true;
      }
    }
    const std::string name =
        n.name.empty() ? ("_n" + std::to_string(n.id)) : n.name;
    line = decl + " " + name + " = " + n.kind + "(";
    bool first = true;
    for (const ValueRef& vr : n.operands) {
      if (!first) line += ", ";
      first = false;
      line += refText(m, vr);
    }
    for (const auto& [key, val] : n.attrs) {
      if (type_promoted && key == "type") continue;
      if (!first) line += ", ";
      first = false;
      line += key + "=" + attrValueText(val);
    }
    line += ");";
  }

  line += "  // n" + std::to_string(n.id);
  return line;
}

std::string assumeLine(const std::string& fact_line) {
  std::string body = fact_line;
  if (body.rfind("fact ", 0) == 0) body = "assume " + body.substr(5);
  return body + ";\n";
}

// Scopes of any sort render as blocks: namespaces, functions, control bodies.
std::string renderScope(const Module& m, const Node& n,
                        const std::string& indent) {
  std::string out;
  if (n.kind == "module") {
    out = indent + "namespace " + n.attrString("sym", "anon") + " {\n";
  } else if (n.kind == "func") {
    out = indent + n.attrString("sym", "anon") + "() {\n";
  } else {
    out = indent + n.kind + " " +
          (n.name.empty() ? ("_n" + std::to_string(n.id)) : n.name) + " {\n";
  }
  for (const Module::FactEntry& f : m.facts) {
    if (f.scope == n.id) out += indent + "  " + assumeLine(f.line);
  }
  for (const NodeId cid : m.childrenOf(n.id)) {
    const Node* c = m.find(cid);
    if (!c) continue;
    out += c->has_body ? renderScope(m, *c, indent + "  ")
                       : indent + "  " + renderNode(m, *c) + "\n";
  }
  out += indent + "}\n\n";
  return out;
}

}  // namespace

std::string toCLike(const Module& m) {
  std::string out =
      "// bridgeir — C-like view (read-only; canonical .br text is the parse "
      "format)\n";

  for (const Module::FactEntry& f : m.facts) {
    if (f.scope == kInvalidId) out += assumeLine(f.line);
  }
  out += "\n";

  for (const NodeId id : m.top_level) {
    const Node* n = m.find(id);
    if (!n) continue;
    if (n->has_body) {
      out += renderScope(m, *n, "");
    } else {
      out += renderNode(m, *n) + "\n";
    }
  }
  return out;
}

}  // namespace bridgeir::io
