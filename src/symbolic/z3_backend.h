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
