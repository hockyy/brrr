// Type terms are data: a name applied to arguments, or a unification variable.
// This is the substrate for math-like inference (rules are data, solving is
// unification) — see docs/scratch/symbols-and-types.md.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bridgeir::types {

struct TypeTerm;
using TypeRef = std::shared_ptr<const TypeTerm>;

struct TypeTerm {
  enum class Kind { Var, Name };
  Kind kind = Kind::Name;
  uint32_t var = 0;           // Kind::Var
  std::string name;           // Kind::Name: "tensor", "f16", "64", ...
  std::vector<TypeRef> args;  // Kind::Name

  static TypeRef makeVar(uint32_t id);
  static TypeRef makeName(std::string name, std::vector<TypeRef> args = {});
};

bool structurallyEqual(const TypeRef& a, const TypeRef& b);

// Robinson unification with occurs check and path-aware diagnostics.
class Unifier {
 public:
  TypeRef fresh();  // a new logic variable (prints as A, B, C, ...)

  // Unify two terms. On failure returns false and fills *error with the failing
  // equation and the path to it.
  bool unify(const TypeRef& a, const TypeRef& b, std::string* error);

  // Fully apply the current substitution.
  TypeRef resolve(const TypeRef& t) const;

  // Resolved, human-readable form: tensor<f32, 64, B>
  std::string pretty(const TypeRef& t) const;

 private:
  bool unifyImpl(const TypeRef& a, const TypeRef& b, const std::string& ctx,
                 std::string* error);
  bool occurs(uint32_t var, const TypeRef& t) const;
  TypeRef resolveShallow(const TypeRef& t) const;
  void bind(uint32_t var, const TypeRef& t);

  std::vector<TypeRef> bindings_;  // var -> term (nullptr = unbound)
  uint32_t next_var_ = 0;
};

}  // namespace bridgeir::types
