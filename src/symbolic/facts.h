// Fact lines are part of the IR document (first-hand symbols):
//
//   fact N = 512                    equality
//   fact aligned(dst_size, 16)      divisibility (symbol on the left)
//   fact range(i, 0, 1023)          bounds
//
// They are consistency-checked against the store as they are applied: a fact that
// contradicts existing facts is a hard error, not a silent overwrite.
#pragma once

#include <string>
#include <vector>

#include "symbolic/symbolic.h"

namespace bridgeir::symbolic {

// Apply one `fact ...` line (including the keyword). Returns false and sets *err
// when malformed or inconsistent with already-applied facts.
bool applyFact(const std::string& line, Solver& s, std::string* err);

bool applyFacts(const std::vector<std::string>& lines, Solver& s,
                std::string* err);

}  // namespace bridgeir::symbolic
