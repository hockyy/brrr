// Exact satisfiability of linear integer constraints, by the Omega test's
// elimination (Pugh, 1991). Only the decision procedure: no dependence analysis,
// no polyhedra. This is the solver's exact linear tier: it decides the
// quasi-affine fragment (linear terms; floordiv/ceildiv/mod by constants become
// fresh variables with their defining bounds) without Z3, deterministically.
//
//   equalities    eliminated by exact substitution; a coefficient that is not
//                 +-1 is first reduced Euclid-style by a unimodular change of
//                 variables
//   inequalities  Fourier-Motzkin: the real shadow refutes, the dark shadow
//                 proves, and splinters cover the gap between them
//
// `Unknown` means the work budget ran out, a subproblem grew past the size cap
// (Fourier-Motzkin multiplies lower by upper bounds), or a coefficient would
// overflow; it is never a guess. The solver then asks the next tier.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace brrr::symbolic::presburger {

using Coeffs = std::map<std::string, int64_t>;

// sum(coeffs[v] * v) + c  == 0  (eq)  or  >= 0  (otherwise).
struct Constraint {
  Coeffs coeffs;
  int64_t c = 0;
  bool eq = false;
};

enum class Sat { Sat, Unsat, Unknown };

// Is there an integer assignment satisfying every constraint? `budget` bounds
// the number of subproblems explored, `max_constraints` the size of each.
Sat satisfiable(std::vector<Constraint> constraints, size_t budget = 2000,
                size_t max_constraints = 256);

}  // namespace brrr::symbolic::presburger
