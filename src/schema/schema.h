// The schema registry: the language's definition.
//
// Kinds are declared as data — named ports, a results policy, typed attributes,
// optional inheritance (`kind relu < unary`) and traits (interfaces). Scalars are
// primitive types, also declared as data. Everything a checker, a pass, or an
// agent needs to know about a kind lives here, not in stringly conventions buried
// in pass code.
//
// Grammar and policies: docs/scratch/schema-registry.md.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bridgeir::schema {

struct Kind {
  std::string name;
  std::string parent;  // `kind X < Parent`

  // Operands: positional, named. `operands: none` is explicit; a trailing `*`
  // marks a variadic tail.
  bool operands_specified = false;
  bool operands_none = false;
  bool operands_variadic = false;
  std::vector<std::string> operands;

  // Results: `value` (default) or `none` (terminator; may not be used as operand).
  bool results_specified = false;
  bool results_value = true;

  // Attributes: Free (default) | None | Closed set with types
  // (int, float, str, sym, dtype, expr, dims).
  enum class AttrPolicy { Free, None, Closed };
  bool attrs_specified = false;
  AttrPolicy attr_policy = AttrPolicy::Free;
  std::map<std::string, std::string> attrs;  // name -> type

  // Interfaces: dispatchable by rules and checkers as effect handling lands.
  std::vector<std::string> traits;
};

// Primitive data types are declared, not assumed.
struct Scalar {
  std::string name;
  std::string domain;  // int | uint | float | bool | index
  int64_t bits = 0;    // 0 = unspecified
};

class Registry {
 public:
  const Kind* find(const std::string& name) const;
  // Resolve `< Parent` by copying unspecified fields from the parent.
  bool add(Kind k, std::string* err);

  const Scalar* findScalar(const std::string& name) const;
  bool addScalar(Scalar s, std::string* err);

  const std::map<std::string, Kind>& kinds() const { return kinds_; }
  const std::map<std::string, Scalar>& scalars() const { return scalars_; }

 private:
  std::map<std::string, Kind> kinds_;
  std::map<std::string, Scalar> scalars_;
};

bool parseSchema(const std::string& text, Registry* reg, std::string* err);

// Compose the standard vocabulary image. `std_kinds` and `control_kinds` load the
// optional modules; with both false only the core module is present.
Registry buildRegistry(bool std_kinds, bool control_kinds);

std::string coreSchemaText();
const Registry& coreRegistry();

}  // namespace bridgeir::schema
