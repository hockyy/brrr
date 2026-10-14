// The MLIR affine bridge: affine maps <-> expressions, both directions exact.
//
//   import   affine_map<(d0, d1)[s0] -> (d0 * 4 + s0, d1)>  applied to (%i, %j)[%n]
//            ->  %i * 4 + %n, %j
//   export   %i * 4 + %n  ->  affine_map<(d0)[s0] -> (d0 * 4 + s0)>  with (%i)[%n]
//
// MLIR's affine expressions are Brrr expressions over d/s atoms (same
// operators, same precedence), so import is parse-and-substitute. Export splits
// atoms into dims and symbols (the caller decides, from SSA: a value defined at
// function entry is a symbol) and checks affinity: every product has at most one
// non-constant factor. `%n * %m` is a fine expression but not affine; export
// refuses it and names the term.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "symbolic/symbolic.h"

namespace brrr::interop {

struct AffineMap {
  std::string text;                  // affine_map<(d0)[s0] -> (d0 * 4 + s0)>
  std::vector<std::string> dims;     // the atoms bound to d0, d1, ...
  std::vector<std::string> symbols;  // ... and to s0, s1, ...
};

// Atoms for which `isSymbol` holds become symbols, the rest dims, in order of
// first appearance.
// `name` spells atoms in error messages (e.g. `%3` -> `%n`).
bool toAffineMap(const std::vector<symbolic::Expr>& results,
                 const std::function<bool(const std::string&)>& isSymbol, AffineMap* out,
                 std::string* err,
                 const std::function<std::string(const std::string&)>& name = {});

// How many dims and symbols a map declares.
bool affineArity(const std::string& map, size_t* dims, size_t* symbols, std::string* err);

// Apply a map (with or without the `affine_map<...>` wrapper) to operands spelled
// as atoms (`%i`, `%n`): dims first, then symbols.
bool fromAffineMap(const std::string& map, const std::vector<std::string>& dims,
                   const std::vector<std::string>& symbols, std::vector<symbolic::Expr>* out,
                   std::string* err);

}  // namespace brrr::interop
