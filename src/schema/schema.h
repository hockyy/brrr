// The schema registry: the vocabulary of the language, as data.
//
// A kind declares its operands, successors, results, attributes, regions and
// traits, and optionally what its result *means* (`expr:`) and how operand shapes
// relate (`shape:`). Checkers, the solver and passes read these declarations; no
// kind needs C++ to be understood.
//
//   kind add {
//     operands: lhs, rhs
//     expr: lhs + rhs              # integer meaning, for the solver
//     shape: S, S -> S             # S = a whole shape
//     traits: pure
//   }
//   kind icmp {
//     operands: lhs, rhs
//     attrs: pred: enum(eq|ne|lt|le|gt|ge)
//   }
//   scalar f16 { domain: float  bits: 16 }
//
// Grammar and the standard vocabulary: docs/scratch/schema-registry.md.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace brrr::schema {

struct Kind {
  std::string name;
  std::string parent;  // `kind X < Parent` copies unspecified fields

  // Operands: positional and named; a trailing `*` makes the last one variadic.
  bool operands_specified = false;
  std::vector<std::string> operands;
  bool operands_variadic = false;

  // Successors: how many `^block(...)` targets (-1 = any number).
  bool successors_specified = false;
  int successors = 0;

  // Results: exact count, or variadic (`value*`).
  bool results_specified = false;
  int results = 1;
  bool results_variadic = false;

  // Attributes: `free` (default) | `none` | a closed list of `name: type`, where a
  // trailing `?` on the name makes it optional. Types: int, float, bool, str, type,
  // array, any, enum(a|b|...).
  enum class AttrPolicy { Free, None, Closed };
  bool attrs_specified = false;
  AttrPolicy attr_policy = AttrPolicy::Free;
  struct Attr {
    std::string type;
    bool optional = false;
  };
  std::map<std::string, Attr> attrs;

  bool regions_specified = false;
  int regions = 0;

  std::string result_type;  // fixed type of result 0 (`type: index`)
  std::string expr;   // symbolic meaning of result 0 (see sema)
  std::string shape;  // shape rule (see sema)
  std::vector<std::string> traits;

  bool hasTrait(const std::string& t) const;
};

struct Scalar {
  std::string name;
  std::string domain;  // int | uint | float | bool | index | ptr
  int64_t bits = 0;    // 0 = unspecified
};

class Registry {
 public:
  const Kind* find(const std::string& name) const;
  bool add(Kind k, std::string* err);  // resolves `< Parent`

  const Scalar* findScalar(const std::string& name) const;
  bool addScalar(Scalar s, std::string* err);

  const std::map<std::string, Kind>& kinds() const { return kinds_; }
  const std::map<std::string, Scalar>& scalars() const { return scalars_; }

 private:
  std::map<std::string, Kind> kinds_;
  std::map<std::string, Scalar> scalars_;
};

bool parseSchema(const std::string& text, Registry* reg, std::string* err);
// Canonical text of one kind (what `brrr schema` prints).
std::string printKind(const Kind& k);

// The standard vocabulary ships as modules: `core` (scalars, return), `std`
// (arithmetic, tensors), `control` (branches, loops), `mem` (buffers, loaded
// with std).
std::string coreSchemaText();
std::string stdSchemaText();
std::string controlSchemaText();
std::string memSchemaText();
Registry buildRegistry(bool std_kinds, bool control_kinds);

// Structural kinds are part of the language, not the vocabulary.
bool isStructural(const std::string& kind);  // module | func | block | arg

}  // namespace brrr::schema
