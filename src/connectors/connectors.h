// The connector hub: queries are DATA, so they cross language borders.
//
// Route: the local solver (canonical form, intervals, residues) decides the affine
// fragment in-process; what it cannot decide escalates to capability-matched
// external connectors — out of process, budgeted, cached by content, and
// attributed so provenance always knows who answered.
#pragma once

#include <map>
#include <string>

#include "symbolic/symbolic.h"

namespace bridgeir::connectors {

using symbolic::Expr;
using symbolic::Solver;
using symbolic::Tri;

// An answer to a symbolic query, attributed to its decider.
struct Answer {
  Tri tri = Tri::Unknown;
  std::string source;  // "local" | "connector@sympy" | ...
  std::string detail;  // justification, cache note, or failure reason
};

class Hub {
 public:
  Hub();

  bool sympyAvailable() const { return sympy_available_; }
  int externalCalls() const { return external_calls_; }

  // Tri-state equality with escalation. Never guesses: Unknown is honest.
  Answer isEqual(const Solver& facts, const Expr& a, const Expr& b);

 private:
  Answer askSympyEqual(const Expr& a, const Expr& b);

  bool sympy_available_ = false;
  int external_calls_ = 0;
  std::map<std::string, Answer> cache_;
};

}  // namespace bridgeir::connectors
