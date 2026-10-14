// Z3 behind the symbolic layer: facts become Z3 assertions over mathematical
// integers, and every query is a validity check.
//
//   Yes      facts  ∧ ¬q  is unsatisfiable      (q holds for every input)
//   No       facts  ∧  q  is unsatisfiable      (q holds for no input)
//   Unknown  anything else, including a timeout or an error
//
// floordiv/mod by a positive constant are SMT-LIB `div`/`mod` (which floor for a
// positive divisor); ceildiv(x, c) is -((-x) div c). Atoms are Int constants
// named by their atom spelling (`%7`, `tile`, `?%7.0`).
//
// Determinism: every check runs on a fresh solver, given the facts in a fixed
// order, under a deterministic resource limit (`rlimit`) and a fixed seed. An
// answer is therefore a function of (Z3 version, limit, facts, question) alone:
// not of machine load, and not of the questions asked before it. A wall-clock
// safety net remains far above the limit; an answer it cuts short is counted as
// nondeterministic (`stats().safety_net_hits`).
//
// Built only when CMake finds Z3 (BRRR_HAS_Z3); otherwise `available()` is
// false and every query is Unknown.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "symbolic/symbolic.h"

namespace brrr::symbolic::z3 {

bool available();
std::string version();

// The per-check resource limit: BRRR_SOLVER_RLIMIT, else the calibrated default
// (docs/scratch/solver.md). 0 means unlimited.
uint64_t rlimit();
void setRlimit(uint64_t limit);

// Counters over the process, for calibration and reports (BRRR_SOLVER_STATS=1).
struct Stats {
  uint64_t checks = 0;
  uint64_t last_rlimit_used = 0;   // what the latest check consumed
  uint64_t max_rlimit_used = 0;    // the most any single check consumed
  uint64_t resource_unknowns = 0;  // checks stopped by the rlimit (deterministic)
  uint64_t safety_net_hits = 0;    // checks stopped by the wall clock (nondeterministic)
};
const Stats& stats();

class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void assertEqual(const Expr& a, const Expr& b);
  void assertLe(const Expr& a, const Expr& b);
  void assertRange(const std::string& atom, int64_t lo, int64_t hi);
  void assertMod(const std::string& atom, int64_t modulus, int64_t residue);

  Tri isEqual(const Expr& a, const Expr& b);
  Tri lessOrEqual(const Expr& a, const Expr& b);
  Tri isLess(const Expr& a, const Expr& b);
  Tri aligned(const Expr& e, int64_t modulus);
  std::optional<int64_t> constantValue(const Expr& e);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Context-free algebraic simplification (Z3's `simplify`), translated back into
// our expression language; nullptr when the result leaves it.
Expr simplify(const Expr& e);

}  // namespace brrr::symbolic::z3
