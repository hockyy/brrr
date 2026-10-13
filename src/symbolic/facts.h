// Fact lines are part of the IR document (first-hand symbols), and they are
// SCOPED: an invariant holds where it is written (a loop body, a block, a func)
// and is visible to inner scopes through the scope chain.
//
//   fact N = 512                    equality
//   fact i <= n                     inequality (strict `<` is not supported yet)
//   fact aligned(dst_size, 16)      divisibility (symbol on the left)
//   fact range(i, 0, 1023)          bounds (integer literals)
//
// They are consistency-checked against the store as they are applied: a fact that
// contradicts existing facts in its scope chain is a hard error.
#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"
#include "symbolic/symbolic.h"

namespace bridgeir::symbolic {

// Apply one `fact ...` line (including the keyword). Returns false and sets *err
// when malformed or inconsistent with already-applied facts.
bool applyFact(const std::string& line, Solver& s, std::string* err);

bool applyFacts(const std::vector<std::string>& lines, Solver& s,
                std::string* err);

// Scoped invariants: validate every fact against its scope chain (root -> scope),
// and build the solver a query in that scope sees.
bool validateAllFacts(const bridgeir::Module& m, std::string* err);
Solver solverForScope(const bridgeir::Module& m, bridgeir::NodeId scope,
                      std::string* err);

}  // namespace bridgeir::symbolic
