#include "io/json.h"

#include <cstdio>

namespace bridgeir::io {
namespace {

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

std::string attrJson(const AttrValue& v) {
  switch (v.kind) {
    case AttrValue::Kind::Int:
      return "{\"type\": \"int\", \"value\": " + std::to_string(v.i) + "}";
    case AttrValue::Kind::Float: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", v.f);
      return std::string("{\"type\": \"float\", \"value\": ") + buf + "}";
    }
    case AttrValue::Kind::Str:
      return "{\"type\": \"str\", \"value\": \"" + jsonEscape(v.s) + "\"}";
    case AttrValue::Kind::Sym:
      return "{\"type\": \"sym\", \"value\": \"" + jsonEscape(v.s) + "\"}";
  }
  return "{}";
}

}  // namespace

std::string toJson(const Module& m) {
  std::string out;
  out += "{\n";
  out += "  \"format\": \"bridgeir\",\n  \"version\": 0,\n";

  out += "  \"facts\": [";
  for (size_t i = 0; i < m.facts.size(); ++i) {
    if (i) out += ", ";
    out += "{\"scope\": " + std::to_string(m.facts[i].scope) +
           ", \"text\": \"" + jsonEscape(m.facts[i].line) + "\"}";
  }
  out += "],\n";

  out += "  \"nodes\": [\n";
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const Node& n = m.nodes[i];
    out += "    {\"id\": " + std::to_string(n.id) + ", \"kind\": \"" +
           jsonEscape(n.kind) + "\"";
    if (!n.name.empty()) out += ", \"name\": \"" + jsonEscape(n.name) + "\"";
    out += ", \"scope\": " + std::to_string(n.scope);
    if (n.has_body) out += ", \"has_body\": true";
    out += ", \"operands\": [";
    for (size_t j = 0; j < n.operands.size(); ++j) {
      if (j) out += ", ";
      out += "{\"id\": " + std::to_string(n.operands[j].id) +
             ", \"port\": " + std::to_string(n.operands[j].port) + "}";
    }
    out += "], \"attrs\": {";
    bool first = true;
    for (const auto& [key, val] : n.attrs) {
      if (!first) out += ", ";
      first = false;
      out += "\"" + jsonEscape(key) + "\": " + attrJson(val);
    }
    out += "}}";
    if (i + 1 < m.nodes.size()) out += ",";
    out += "\n";
  }
  out += "  ]\n}\n";
  return out;
}

}  // namespace bridgeir::io
