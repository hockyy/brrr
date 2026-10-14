#include "schema/schema.h"

#include <algorithm>
#include <sstream>

namespace brrr::schema {
namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Commas separate list items; parentheses group (`enum(a|b)`).
std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (const char c : s) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == ',' && depth == 0) {
      if (!trim(cur).empty()) out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
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

bool validAttrType(const std::string& t) {
  static const char* const kTypes[] = {"int", "float", "bool", "str",
                                       "type", "array", "any"};
  for (const char* k : kTypes) {
    if (t == k) return true;
  }
  return t.rfind("enum(", 0) == 0 && t.back() == ')' && t.size() > 6;
}

}  // namespace

bool Kind::hasTrait(const std::string& t) const {
  return std::find(traits.begin(), traits.end(), t) != traits.end();
}

bool isStructural(const std::string& kind) {
  return kind == "module" || kind == "func" || kind == "block" || kind == "arg";
}

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
  if (isStructural(k.name)) {
    *err = "'" + k.name + "' is part of the language, not a kind";
    return false;
  }
  if (!k.parent.empty()) {
    auto it = kinds_.find(k.parent);
    if (it == kinds_.end()) {
      *err = "unknown parent kind '" + k.parent + "'";
      return false;
    }
    const Kind& base = it->second;
    if (!k.operands_specified) {
      k.operands_specified = base.operands_specified;
      k.operands = base.operands;
      k.operands_variadic = base.operands_variadic;
    }
    if (!k.successors_specified) {
      k.successors_specified = base.successors_specified;
      k.successors = base.successors;
    }
    if (!k.results_specified) {
      k.results_specified = base.results_specified;
      k.results = base.results;
      k.results_variadic = base.results_variadic;
    }
    if (!k.attrs_specified) {
      k.attrs_specified = base.attrs_specified;
      k.attr_policy = base.attr_policy;
      k.attrs = base.attrs;
    }
    if (!k.regions_specified) {
      k.regions_specified = base.regions_specified;
      k.regions = base.regions;
    }
    if (k.result_type.empty()) k.result_type = base.result_type;
    if (k.expr.empty()) k.expr = base.expr;
    if (k.shape.empty()) k.shape = base.shape;
    std::vector<std::string> traits = base.traits;
    for (const std::string& t : k.traits) {
      if (std::find(traits.begin(), traits.end(), t) == traits.end()) traits.push_back(t);
    }
    k.traits = std::move(traits);
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
    auto close = [&]() {
      const bool ok = state == State::Kind ? reg->add(std::move(cur), err)
                                           : reg->addScalar(std::move(cur_scalar), err);
      state = State::None;
      return ok;
    };

    if (state == State::None) {
      const bool empty_body = line.size() >= 2 && line.substr(line.size() - 2) == "{}";
      if (line.back() != '{' && !empty_body) {
        return fail("expected 'kind NAME {' or 'scalar NAME {'");
      }
      const std::string head =
          trim(line.substr(0, line.size() - (empty_body ? 2 : 1)));
      if (head.rfind("kind ", 0) == 0) {
        const std::string inner = trim(head.substr(5));
        cur = Kind{};
        const size_t lt = inner.find('<');
        cur.name = trim(inner.substr(0, lt));
        if (lt != std::string::npos) cur.parent = trim(inner.substr(lt + 1));
        if (cur.name.empty()) return fail("empty kind name");
        state = State::Kind;
      } else if (head.rfind("scalar ", 0) == 0) {
        cur_scalar = Scalar{};
        cur_scalar.name = trim(head.substr(7));
        if (cur_scalar.name.empty()) return fail("empty scalar name");
        state = State::Scalar;
      } else {
        return fail("expected 'kind NAME {' or 'scalar NAME {'");
      }
      if (empty_body && !close()) return fail(*err);
      continue;
    }

    if (line == "}") {
      if (!close()) return fail(*err);
      continue;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) return fail("expected 'field: value'");
    const std::string key = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));

    if (state == State::Scalar) {
      if (key == "domain") {
        static const char* const kDomains[] = {"int", "uint", "float", "bool", "index", "ptr"};
        if (std::none_of(std::begin(kDomains), std::end(kDomains),
                         [&](const char* d) { return value == d; })) {
          return fail("domain must be int|uint|float|bool|index|ptr");
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
      cur.operands_specified = true;
      if (value != "none") {
        for (std::string slot : splitList(value)) {
          if (slot.back() == '*') {
            cur.operands_variadic = true;
            slot = trim(slot.substr(0, slot.size() - 1));
          }
          if (slot.empty()) return fail("empty operand name");
          cur.operands.push_back(slot);
        }
        if (cur.operands.empty()) return fail("empty operands spec");
      }
    } else if (key == "successors") {
      cur.successors_specified = true;
      int64_t n = 0;
      if (value == "*") {
        cur.successors = -1;
      } else if (parseStrictInt(value, &n) && n >= 0) {
        cur.successors = static_cast<int>(n);
      } else {
        return fail("successors must be a count or '*'");
      }
    } else if (key == "results") {
      cur.results_specified = true;
      int64_t n = 0;
      if (value == "none") {
        cur.results = 0;
      } else if (value == "value") {
        cur.results = 1;
      } else if (value == "value*") {
        cur.results = 0;
        cur.results_variadic = true;
      } else if (parseStrictInt(value, &n) && n >= 0) {
        cur.results = static_cast<int>(n);
      } else {
        return fail("results must be none | value | value* | a count");
      }
    } else if (key == "attrs") {
      cur.attrs_specified = true;
      if (value == "free") {
        cur.attr_policy = Kind::AttrPolicy::Free;
      } else if (value == "none") {
        cur.attr_policy = Kind::AttrPolicy::None;
      } else {
        cur.attr_policy = Kind::AttrPolicy::Closed;
        for (const std::string& part : splitList(value)) {
          const size_t c2 = part.find(':');
          if (c2 == std::string::npos) return fail("attribute '" + part + "' needs a type (name: type)");
          std::string an = trim(part.substr(0, c2));
          const std::string at = trim(part.substr(c2 + 1));
          Kind::Attr a;
          if (!an.empty() && an.back() == '?') {
            a.optional = true;
            an = trim(an.substr(0, an.size() - 1));
          }
          if (an.empty() || !validAttrType(at)) {
            return fail("malformed attribute '" + part +
                        "' (types: int, float, bool, str, type, array, any, enum(a|b))");
          }
          a.type = at;
          cur.attrs[an] = a;
        }
      }
    } else if (key == "regions") {
      int64_t n = 0;
      if (!parseStrictInt(value, &n) || n < 0) return fail("regions must be a count");
      cur.regions_specified = true;
      cur.regions = static_cast<int>(n);
    } else if (key == "type") {
      if (value.empty()) return fail("empty type");
      cur.result_type = value;
    } else if (key == "expr") {
      if (value.empty()) return fail("empty expr");
      cur.expr = value;
    } else if (key == "shape") {
      if (value.find("->") == std::string::npos) return fail("shape rule needs '->'");
      cur.shape = value;
    } else if (key == "traits") {
      for (const std::string& t : splitList(value)) cur.traits.push_back(t);
      if (cur.traits.empty()) return fail("empty traits spec");
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

std::string printKind(const Kind& k) {
  std::string out = "kind " + k.name + (k.parent.empty() ? "" : " < " + k.parent) + " {\n";
  if (k.operands_specified) {
    std::string ops;
    for (size_t i = 0; i < k.operands.size(); ++i) {
      if (i) ops += ", ";
      ops += k.operands[i];
    }
    if (k.operands_variadic) ops += "*";
    out += "  operands: " + (ops.empty() ? std::string("none") : ops) + "\n";
  }
  if (k.successors != 0) {
    out += "  successors: " + (k.successors < 0 ? std::string("*") : std::to_string(k.successors)) + "\n";
  }
  if (k.results_variadic) {
    out += "  results: value*\n";
  } else if (k.results != 1) {
    out += "  results: " + (k.results == 0 ? std::string("none") : std::to_string(k.results)) + "\n";
  }
  if (k.attr_policy == Kind::AttrPolicy::None) {
    out += "  attrs: none\n";
  } else if (k.attr_policy == Kind::AttrPolicy::Closed) {
    std::string s;
    for (const auto& [an, a] : k.attrs) {
      if (!s.empty()) s += ", ";
      s += an + (a.optional ? "?" : "") + ": " + a.type;
    }
    out += "  attrs: " + s + "\n";
  }
  if (k.regions) out += "  regions: " + std::to_string(k.regions) + "\n";
  if (!k.result_type.empty()) out += "  type: " + k.result_type + "\n";
  if (!k.expr.empty()) out += "  expr: " + k.expr + "\n";
  if (!k.shape.empty()) out += "  shape: " + k.shape + "\n";
  if (!k.traits.empty()) {
    std::string s;
    for (const std::string& t : k.traits) s += (s.empty() ? "" : ", ") + t;
    out += "  traits: " + s + "\n";
  }
  return out + "}\n";
}

// ---------------------------------------------------------------------------
// The standard vocabulary. Everything above the language skeleton is a module.
// ---------------------------------------------------------------------------

std::string coreSchemaText() {
  return R"SCHEMA(# core module: primitive scalars and the function terminator.
scalar bool {
  domain: bool
  bits: 1
}
scalar i1 {
  domain: int
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
scalar u8 {
  domain: uint
  bits: 8
}
scalar u16 {
  domain: uint
  bits: 16
}
scalar u32 {
  domain: uint
  bits: 32
}
scalar u64 {
  domain: uint
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
scalar ptr {
  domain: ptr
  bits: 64
}

kind return {
  operands: values*
  results: none
  traits: terminator
}

# A placeholder conversion between type systems, like MLIR's
# builtin.unrealized_conversion_cast: allowed while an import or a lowering is in
# progress, opaque to the solver, and gone by the end (`reconcile-casts` folds
# round trips; every one left is a warning, an error under `check --strict`).
kind unrealized_cast {
  operands: inputs*
  results: value*
  traits: pure, unrealized
}
)SCHEMA";
}

std::string stdSchemaText() {
  return R"SCHEMA(# std module: arithmetic and tensors.
# `expr:` is what the result means to the solver; `shape:` relates operand shapes.

kind const {
  operands: none
  attrs: value: any
  expr: @value
  traits: pure
}

# Integer meanings need "cannot wrap": always for index, and for iN/uN only when
# the op says so (nsw = no signed wrap, nuw = no unsigned wrap).
kind binary {
  operands: lhs, rhs
  attrs: nsw?: bool, nuw?: bool
  shape: S, S -> S
  traits: pure
}
kind add < binary {
  expr: lhs + rhs
}
kind sub < binary {
  expr: lhs - rhs
}
kind mul < binary {
  expr: lhs * rhs
}

# An integer expression, as a value: `%m = apply %base * 2 floordiv 3`. The
# analog of MLIR's affine.apply, without a map: the operand is the formula.
kind apply {
  operands: formula
  type: index
  expr: formula
  traits: pure
}

kind icmp {
  operands: lhs, rhs
  attrs: pred: enum(eq|ne|lt|le|gt|ge)
  type: i1
  traits: pure
}

kind select {
  operands: cond, if_true, if_false
  traits: pure
}

kind dim {
  operands: source
  attrs: index: int
  type: index
  expr: dim(source, @index)
  traits: pure
}

kind unary {
  operands: x
  shape: S -> S
  traits: pure
}
kind relu < unary {
}
# One conversion op: the source and target types decide sign- or zero-extension,
# truncation, or index conversion (on export: arith.extsi / extui / trunci /
# index_cast). It means its operand exactly when every source value fits the
# target type (a widening); otherwise it may wrap, and is opaque to the solver.
kind cast < unary {
  attrs: to: type
  expr: x
  traits: conversion
}

kind reshape {
  operands: source
  traits: pure, same_numel
}

kind matmul {
  operands: lhs, rhs
  shape: [M, K], [K, N] -> [M, N]
  traits: pure
}

kind nonzero {
  operands: x
  shape: [N] -> [?] : index
  traits: pure
}

# A runtime check: execution stops unless lhs == rhs (rel=eq) or lhs <= rhs
# (rel=le). Below a guard its relation is a fact. `insert-guards` writes these
# for equations the solver could not prove.
kind guard {
  operands: lhs, rhs
  results: none
  attrs: rel: enum(eq|le), what?: str
}
)SCHEMA";
}

std::string controlSchemaText() {
  return R"SCHEMA(# control module: branches and loops.
# A successor carries the values for its block's arguments. A `loop` has one
# region whose entry arguments are the carried values; `continue_if` ends the
# region with (condition, next values) and the loop's results are the final values.

kind br {
  operands: none
  successors: 1
  results: none
  traits: terminator
}

kind cond_br {
  operands: cond
  successors: 2
  results: none
  traits: terminator
}

kind loop {
  operands: init*
  results: value*
  regions: 1
  traits: loop
}

kind continue_if {
  operands: cond, next*
  results: none
  traits: terminator, loop.continue
}
)SCHEMA";
}

std::string memSchemaText() {
  return R"SCHEMA(# mem module: buffers.
# A buffer is a shaped type, `buf<[dims], elem>` by convention (the name is free):
# row-major, contiguous, its dims expressions over values. An `access` kind
# indexes a buffer (operand `buf`) with one expression per dimension (operand
# `at`, a list), and every index is checked against the buffer's dims under the
# facts in scope: proven in bounds, or a warning `insert-guards` can guard.

kind alloc {
  operands: none
  attrs: none
}

kind dealloc {
  operands: buf
  results: none
  attrs: none
}

kind load {
  operands: buf, at
  attrs: none
  traits: access
}

kind store {
  operands: value, buf, at
  results: none
  attrs: none
  traits: access
}
)SCHEMA";
}

Registry buildRegistry(bool std_kinds, bool control_kinds) {
  Registry r;
  std::string err;
  parseSchema(coreSchemaText(), &r, &err);  // known-good literals
  if (std_kinds) parseSchema(stdSchemaText(), &r, &err);
  if (std_kinds) parseSchema(memSchemaText(), &r, &err);
  if (control_kinds) parseSchema(controlSchemaText(), &r, &err);
  return r;
}

}  // namespace brrr::schema
