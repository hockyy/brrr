// Expression text: parsing and printing, shared by types (dims), expression
// operands, facts, and `ask` queries.
//
//   expr := int | %value | symbol | '?' | dim(%value, int)
//         | expr ('+'|'-'|'*') expr | '-' expr | '(' expr ')'
//         | expr ('floordiv'|'ceildiv'|'mod') int     (a positive constant)
//
// `floordiv`, `ceildiv` and `mod` bind like `*`, left to right, as in MLIR's
// affine expressions: `%b * 2 floordiv 3` is `(%b * 2) floordiv 3`. There is no
// `/`: integer division must say how it rounds.
//
// Atoms keep their spelling as the variable name: `%acc`, `%12`, `%12#1`, `tile`,
// `?`. `dim(%x, k)` — the k-th dimension of %x — is the atom `?%x.k`; printing
// and renaming see through it. The IR parser renames value atoms to their ids
// (`%acc` -> `%7`).
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "symbolic/symbolic.h"

namespace brrr::symbolic {

// Parse an expression. Returns nullptr and sets *err on malformed input.
Expr parseExpr(const std::string& text, std::string* err);

// Print with minimal parentheses: `2 * %n + 1`, `%i - 1`. `rename` maps a variable
// name to its printed form (e.g. `%7` -> `%acc`); identity when empty.
std::string printExpr(const Expr& e,
                      const std::function<std::string(const std::string&)>& rename = {});

// `?%x.k` (the dim(%x, k) atom) -> value `%x`, dimension k.
bool splitDimAtom(const std::string& atom, std::string* value, size_t* k);

// Rebuild `e` with every variable renamed by `f` (inside dim(...) too).
Expr mapVars(const Expr& e, const std::function<std::string(const std::string&)>& f);

// Every variable name in `e`.
void collectVars(const Expr& e, std::set<std::string>* out);

// Split a top-level comma-separated list (bracket-aware), e.g. a dims list.
std::vector<std::string> splitTopLevel(const std::string& s, char delim);

}  // namespace brrr::symbolic
