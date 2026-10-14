// Facts are part of the IR document, and they are SCOPED: a fact holds where it is
// written (module, function, block, region) and in every scope nested inside.
//
//   fact tile = 128                 equality (binds the module symbol `tile`)
//   fact 0 <= %i <= %n              chained relations: =, ==, <=, >=
//   fact aligned(%n, 16)            divisibility (an atom on the left)
//   fact range(%i, 0, 1023)         bounds (integer literals)
//
// Facts are consistency-checked as they are applied: a fact that contradicts the
// facts already visible in its scope is a hard error.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "symbolic/symbolic.h"

namespace brrr::symbolic {

struct ParsedFact {
  enum class Kind { Relation, Aligned, Range };
  Kind kind = Kind::Relation;
  std::vector<Expr> exprs;       // Relation: sides; Aligned/Range: the subject
  std::vector<std::string> ops;  // Relation: "=", "<=", ">=" between sides
  int64_t a = 0, b = 0;          // Aligned: modulus; Range: lo, hi
};

// Parse a fact body (with or without the leading `fact`).
bool parseFact(const std::string& text, ParsedFact* out, std::string* err);
// Canonical text without the keyword: `0 <= %i <= %n`, `aligned(%n, 16)`.
std::string printFact(const ParsedFact& f,
                      const std::function<std::string(const std::string&)>& rename = {});
// Rename every atom (used to map `%name` <-> `%id`).
ParsedFact mapFactVars(const ParsedFact& f,
                       const std::function<std::string(const std::string&)>& rename);
void collectFactVars(const ParsedFact& f, std::set<std::string>* out);

// Apply one fact. Returns false and sets *err when malformed or inconsistent with
// facts already applied.
bool applyFact(const std::string& text, Solver& s, std::string* err);

// Every scope's facts must be consistent with its enclosing scopes' facts.
bool validateAllFacts(const brrr::Module& m, std::string* err);
// The facts visible in `scope` (root through `scope`), applied in order.
Solver solverForScope(const brrr::Module& m, brrr::NodeId scope,
                      std::string* err);

}  // namespace brrr::symbolic
