#include "schema/schema.h"

#include <algorithm>
#include <sstream>

namespace bridgeir::schema {
namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Schema lists are separated by commas and/or whitespace — both styles appear
// in practice (`operands: lhs, rhs` and `attrs: dtype:dtype shape:dims`).
std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool parseStrictInt(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) return false;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  *out = std::strtoll(s.c_str(), nullptr, 10);
  return true;
}

}  // namespace

const Kind* Registry::find(const std::string& name) const {
  auto it = kinds_.find(name);
  return it == kinds_.end() ? nullptr : &it->second;
}

const Scalar* Registry::findScalar(const std::string& name) const {
  auto it = scalars_.find(name);
  return it == scalars_.end() ? nullptr : &it->second;
}

bool Registry::addScalar(Scalar s, std::string* err) {
  if (scalars_.count(s.name)) {
    *err = "duplicate scalar '" + s.name + "'";
    return false;
  }
  scalars_[s.name] = std::move(s);
  return true;
}

bool Registry::add(Kind k, std::string* err) {
  if (kinds_.count(k.name)) {
    *err = "duplicate kind '" + k.name + "'";
    return false;
  }
  if (!k.parent.empty()) {
    auto it = kinds_.find(k.parent);
    if (it == kinds_.end()) {
      *err = "unknown parent kind '" + k.parent + "'";
      return false;
    }
    const Kind& base = it->second;
    Kind out = k;
    if (!out.operands_specified) {
      out.operands_specified = base.operands_specified;
      out.operands_none = base.operands_none;
      out.operands_variadic = base.operands_variadic;
      out.operands = base.operands;
    }
    if (!out.results_specified) {
      out.results_value = base.results_value;
      out.results_specified = true;
    }
    if (!out.attrs_specified) {
      out.attr_policy = base.attr_policy;
      out.attrs = base.attrs;
      out.attrs_specified = base.attrs_specified;
    }
    out.traits = base.traits;
    for (const std::string& t : k.traits) {
      if (std::find(out.traits.begin(), out.traits.end(), t) == out.traits.end()) {
        out.traits.push_back(t);
      }
    }
    kinds_[out.name] = std::move(out);
    return true;
  }
  kinds_[k.name] = std::move(k);
  return true;
}

bool parseSchema(const std::string& text, Registry* reg, std::string* err) {
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  Kind cur;
  Scalar cur_scalar;
  enum class State { None, Kind, Scalar } state = State::None;

  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;

    auto fail = [&](const std::string& msg) {
      *err = "line " + std::to_string(lineno) + ": " + msg;
      return false;
    };

    if (state == State::None) {
      if (line.rfind("kind ", 0) == 0 && line.back() == '{') {
        const std::string inner = trim(line.substr(5, line.size() - 6));
        std::string name = inner;
        std::string parent;
        const size_t lt = inner.find('<');
        if (lt != std::string::npos) {
          name = trim(inner.substr(0, lt));
          parent = trim(inner.substr(lt + 1));
        }
        if (name.empty()) return fail("empty kind name");
        cur = Kind{};
        cur.name = name;
        cur.parent = parent;
        state = State::Kind;
        continue;
      }
      if (line.rfind("scalar ", 0) == 0 && line.back() == '{') {
        const std::string name = trim(line.substr(7, line.size() - 8));
        if (name.empty()) return fail("empty scalar name");
        cur_scalar = Scalar{};
        cur_scalar.name = name;
        state = State::Scalar;
        continue;
      }
      return fail("expected 'kind NAME {' or 'scalar NAME {'");
    }

    if (line == "}") {
      if (state == State::Kind) {
        if (!reg->add(std::move(cur), err)) return fail(*err);
      } else {
        if (!reg->addScalar(std::move(cur_scalar), err)) return fail(*err);
      }
      state = State::None;
      continue;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) return fail("expected 'field: value'");
    const std::string key = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));

    if (state == State::Scalar) {
      if (key == "domain") {
        if (value != "int" && value != "uint" && value != "float" &&
            value != "bool" && value != "index") {
          return fail("domain must be int|uint|float|bool|index");
        }
        cur_scalar.domain = value;
      } else if (key == "bits") {
        int64_t bits = 0;
        if (!parseStrictInt(value, &bits) || bits <= 0) {
          return fail("bits must be a positive integer");
        }
        cur_scalar.bits = bits;
      } else {
        return fail("unknown scalar field '" + key + "'");
      }
      continue;
    }

    if (key == "operands") {
      if (value.empty()) return fail("empty operands spec");
      cur.operands_specified = true;
      if (value == "none") {
        cur.operands_none = true;
      } else {
        // Slots: commas separate slots; a slot may be `name: type` (spaces around
        // ':' allowed), or bare tokens (names or types) split on whitespace.
        auto toSlots = [&](const std::string& spec,
                           std::vector<std::string>* names,
                           std::vector<std::string>* types) {
          std::vector<std::string> pieces;
          std::string curp;
          for (const char ch : spec) {
            if (ch == ',') {
              pieces.push_back(trim(curp));
              curp.clear();
            } else {
              curp += ch;
            }
          }
          pieces.push_back(trim(curp));
          for (const std::string& piece : pieces) {
            if (piece.empty()) continue;
            const size_t c2 = piece.find(':');
            if (c2 != std::string::npos) {
              names->push_back(trim(piece.substr(0, c2)));
              types->push_back(trim(piece.substr(c2 + 1)));
              continue;
            }
            std::string w;
            for (size_t i = 0; i <= piece.size(); ++i) {
              if (i == piece.size() || piece[i] == ' ' || piece[i] == '\t') {
                if (!w.empty()) {
                  std::string name = w;
                  std::string type;
                  if (w == "value" || w == "block" || w == "edge" ||
                      w == "any") {
                    type = w;
                    name.clear();
                  }
                  names->push_back(name);
                  types->push_back(type);
                  w.clear();
                }
              } else {
                w += piece[i];
              }
            }
          }
        };

        if (value.front() == '(') {
          // Port group: (slot, slot, ...)* — operands repeat as tuples.
          if (value.back() != '*' || value.find(')') == std::string::npos) {
            return fail("tuple operand spec must be ( ... )*");
          }
          const size_t rp = value.rfind(')');
          toSlots(value.substr(1, rp - 1), &cur.operands, &cur.operand_types);
          if (cur.operands.empty()) return fail("empty tuple operand spec");
          cur.operand_tuple_width = cur.operands.size();
        } else {
          std::string spec = value;
          if (!spec.empty() && spec.back() == '*') {
            cur.operands_variadic = true;
            spec.pop_back();
          }
          toSlots(spec, &cur.operands, &cur.operand_types);
        }
        for (const std::string& type : cur.operand_types) {
          if (!type.empty() && type != "value" && type != "block" &&
              type != "edge" && type != "any") {
            return fail("unknown port type '" + type + "'");
          }
        }
        if (cur.operands.empty()) return fail("empty operands spec");
      }
    } else if (key == "results") {
      cur.results_specified = true;
      if (value == "none") {
        cur.results_value = false;
      } else if (value == "value") {
        cur.results_value = true;
      } else {
        return fail("results must be 'value' or 'none'");
      }
    } else if (key == "attrs") {
      cur.attrs_specified = true;
      if (value == "free") {
        cur.attr_policy = Kind::AttrPolicy::Free;
      } else if (value == "none") {
        cur.attr_policy = Kind::AttrPolicy::None;
      } else {
        cur.attr_policy = Kind::AttrPolicy::Closed;
        for (const std::string& part : splitComma(value)) {
          const size_t c2 = part.find(':');
          if (c2 == std::string::npos) {
            return fail("attribute '" + part + "' needs a type (name:type)");
          }
          const std::string an = trim(part.substr(0, c2));
          const std::string at = trim(part.substr(c2 + 1));
          if (an.empty() || at.empty()) return fail("malformed attribute spec");
          cur.attrs[an] = at;
        }
      }
    } else if (key == "traits") {
      if (value.empty()) return fail("empty traits spec");
      for (const std::string& t : splitComma(value)) {
        if (t.empty()) return fail("empty trait name");
        cur.traits.push_back(t);
      }
    } else {
      return fail("unknown field '" + key + "'");
    }
  }

  if (state != State::None) {
    *err = "unterminated block";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The standard vocabulary ships as modules: `core` (skeleton + primitives),
// `std` (common ops), `control` (branches and loops). Everything above the
// language skeleton is a module.
// ---------------------------------------------------------------------------

std::string coreSchemaText() {
  return R"SCHEMA(# core module: the language skeleton + primitive types.
# Kinds: operands are positional and named; `none` | `a, b` | `tail*` (variadic).
# `results: none` marks terminators that may not be referenced as operands.
# Attr types: int, float, str, sym, dtype (declared scalar), expr, dims.
# `kind X < Parent` inherits unspecified fields; `traits` are interfaces.

scalar bool {
  domain: bool
  bits: 1
}
scalar i8 {
  domain: int
  bits: 8
}
scalar i16 {
  domain: int
  bits: 16
}
scalar i32 {
  domain: int
  bits: 32
}
scalar i64 {
  domain: int
  bits: 64
}
scalar f16 {
  domain: float
  bits: 16
}
scalar bf16 {
  domain: float
  bits: 16
}
scalar f32 {
  domain: float
  bits: 32
}
scalar f64 {
  domain: float
  bits: 64
}
scalar index {
  domain: index
  bits: 64
}

kind module {
  operands: none
  results: none
  attrs: sym:sym
}

kind func {
  operands: none
  results: none
  attrs: sym:sym
}

kind param {
  operands: none
  results: value
  traits: arg
}

kind func.return {
  operands: value*
  results: none
}
)SCHEMA";
}

std::string stdSchemaText() {
  return R"SCHEMA(# std module: common operation vocabulary.
kind unary {
  operands: x: value
  results: value
  traits: pure
}

kind load {
  operands: none
  results: value
  attrs: dtype:dtype shape:dims
}

kind reshape {
  operands: source: value
  results: value
  attrs: in:dims out:dims
  traits: pure
}

kind add {
  operands: lhs: value, rhs: value
  results: value
  traits: pure
}

kind relu < unary {
}
)SCHEMA";
}

std::string controlSchemaText() {
  return R"SCHEMA(# control module: branches and loops.
# CFG form: blocks are scopes; `edge` carries branch payloads.
# Simple form: `loop` is a scope whose `param`s are the carried values and whose
# single `loop.yield` supplies (condition, next carried values); the `loop.N`
# result ports are the exit values, in parameter order.

kind block {
  operands: none
  results: value
  traits: block
}

kind edge {
  operands: target: block, passed: value*
  results: value
  traits: edge
}

kind br {
  operands: target: any
  results: none
  traits: terminator
}

kind cbr {
  operands: cond: value, if_true: value, if_false: value
  results: none
  traits: terminator
}

kind select {
  operands: cond: value, if_true: value, if_false: value
  results: value
}

kind phi {
  operands: (value, block)*
  results: value
  traits: phi
}

kind yield {
  operands: value*
  results: none
}

kind loop {
  operands: init: value*
  results: value
  traits: loop
}

kind loop.yield {
  operands: cond: value, next: value*
  results: none
  traits: loop.yield
}

kind icmp {
  operands: lhs: value, rhs: value
  results: value
}

kind arith.constant {
  operands: none
  results: value
}

kind arith.addi {
  operands: lhs: value, rhs: value
  results: value
}
)SCHEMA";
}

Registry buildRegistry(bool std_kinds, bool control_kinds) {
  Registry r;
  std::string err;
  parseSchema(coreSchemaText(), &r, &err);  // known-good literals
  if (std_kinds) parseSchema(stdSchemaText(), &r, &err);
  if (control_kinds) parseSchema(controlSchemaText(), &r, &err);
  return r;
}

const Registry& coreRegistry() {
  static const Registry reg = buildRegistry(true, true);
  return reg;
}

}  // namespace bridgeir::schema
