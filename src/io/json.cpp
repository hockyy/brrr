#include "io/json.h"

#include "symbolic/expr_io.h"

namespace brrr::io {
namespace {

std::string esc(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out + "\"";
}

std::string ref(const ValueRef& v) {
  return "{\"id\": " + std::to_string(v.id) + ", \"port\": " + std::to_string(v.port) + "}";
}

std::string attr(const AttrValue& v) {
  static const char* const kKinds[] = {"int", "float", "bool", "str", "ident", "type", "array"};
  std::string out = "{\"kind\": \"" + std::string(kKinds[static_cast<int>(v.kind)]) +
                    "\", \"text\": " + esc(v.toString());
  if (v.kind == AttrValue::Kind::Int) out += ", \"value\": " + std::to_string(v.i);
  return out + "}";
}

}  // namespace

std::string toJson(const Module& m) {
  std::string out = "{\n  \"format\": \"brrr\",\n  \"version\": 1,\n  \"facts\": [";
  for (size_t i = 0; i < m.facts.size(); ++i) {
    out += (i ? ", " : "") + std::string("{\"scope\": ") + std::to_string(m.facts[i].scope) +
           ", \"text\": " + esc(m.facts[i].text) + "}";
  }
  out += "],\n  \"nodes\": [\n";
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const Node& n = m.nodes[i];
    out += "    {\"id\": " + std::to_string(n.id) + ", \"kind\": " + esc(n.kind) +
           ", \"scope\": " + std::to_string(n.scope);
    if (!n.symbol.empty()) out += ", \"symbol\": " + esc(n.symbol);
    if (n.is_scope) out += ", \"is_scope\": true";
    if (!n.names.empty()) {
      out += ", \"names\": [";
      for (size_t k = 0; k < n.names.size(); ++k) out += (k ? ", " : "") + esc(n.names[k]);
      out += "]";
    }
    if (!n.types.empty()) {
      out += ", \"types\": [";
      for (size_t k = 0; k < n.types.size(); ++k) {
        out += (k ? ", " : "") + (n.types[k] ? esc(printType(n.types[k])) : "null");
      }
      out += "]";
    }
    if (!n.operands.empty()) {
      out += ", \"operands\": [";
      for (size_t k = 0; k < n.operands.size(); ++k) {
        const Operand& o = n.operands[k];
        out += k ? ", " : "";
        if (o.kind == Operand::Kind::Value) {
          out += "{\"value\": " + ref(o.value) + "}";
        } else if (o.kind == Operand::Kind::Expr) {
          out += "{\"expr\": " + esc(symbolic::printExpr(o.exprs[0])) + "}";
        } else {
          out += "{\"list\": [";
          for (size_t j = 0; j < o.exprs.size(); ++j) {
            out += (j ? ", " : "") + esc(symbolic::printExpr(o.exprs[j]));
          }
          out += "]}";
        }
      }
      out += "]";
    }
    if (!n.successors.empty()) {
      out += ", \"successors\": [";
      for (size_t k = 0; k < n.successors.size(); ++k) {
        out += (k ? ", " : "") + std::string("{\"block\": ") +
               std::to_string(n.successors[k].block) + ", \"args\": [";
        for (size_t j = 0; j < n.successors[k].args.size(); ++j) {
          out += (j ? ", " : "") + ref(n.successors[k].args[j]);
        }
        out += "]}";
      }
      out += "]";
    }
    if (!n.attrs.empty()) {
      out += ", \"attrs\": {";
      bool first = true;
      for (const auto& [key, val] : n.attrs) {
        out += (first ? "" : ", ") + esc(key) + ": " + attr(val);
        first = false;
      }
      out += "}";
    }
    out += "}";
    out += i + 1 < m.nodes.size() ? ",\n" : "\n";
  }
  return out + "  ]\n}\n";
}

}  // namespace brrr::io
