# The solver

Every shape equation, fact, and expression question in Brrr goes through one
interface, `symbolic::Solver` (`src/symbolic/symbolic.h`). Callers ask for
`isEqual`, `lessOrEqual`, `isLess`, `aligned` or `constantValue` and get back
`Yes`, `No` or `Unknown`. Nothing outside `symbolic/` knows which engine
answered.

## Answers

| Answer | Meaning |
|---|---|
| `Yes` | the question holds for **every** input that satisfies the facts in scope |
| `No` | it holds for **no** such input |
| `Unknown` | neither was shown, which includes hitting the resource limit and expressions outside the language |

An `Unknown` is never treated as `Yes`. Verification reports it as a warning
(an error under `--strict`), and `insert-guards` can turn it into a runtime
`guard`.

## Tiers

A question goes through tiers in order. Every tier is sound, so the first Yes or
No wins:

| Tier | What it does | Cost |
|---|---|---|
| theorem | identical polynomial normal forms give Yes; a nonzero constant difference gives No | microseconds |
| local | the hand-written rules: substitution from equality facts, linear `<=` facts, intervals with sound products, residues, division bounds, and sums of non-negative monomials (`0 <= %s * %s * %b * %b` holds because every power is even) | microseconds |
| exact | **linear integer arithmetic decided exactly** (`presburger.cpp`, the Omega test's elimination). A `floordiv`, `ceildiv` or `mod` by a constant becomes a fresh variable with its defining bounds; a product of non-constants becomes an opaque atom | about a millisecond |
| z3 | Z3, only for what the tiers above leave open. Linear questions go to its `QF_LIA` solver, nonlinear ones to the general strategy | milliseconds and up |

The exact tier also knows when an Unknown is **final**. If it saw the whole
problem (no fact dropped, no opaque atom, no limit hit) and found the question
satisfiable both ways, then the answer depends on the input, and no solver can
say Yes or No. Z3 is not asked. Linear questions therefore never reach Z3, and
the solver stays complete on the linear fragment even when Brrr is built without
Z3.

On the test suite and examples, the tiers change where answers come from:

| | before | after |
|---|---|---|
| questions answered by Z3 | 45 (of 124) | 5 |
| Z3 checks | 120 | 10 |
| costliest Z3 check (rlimit) | ~570k | 410 |

The exact tier is capped: at most 2000 subproblems, each with at most 256
constraints (Fourier–Motzkin multiplies lower bounds by upper bounds). Past
either cap it gives an honest Unknown, and Z3 answers instead.

**How it is kept honest.** `check` mode asks the local tiers and Z3 on every
question. It reports any Yes/No disagreement, and also any *settled* Unknown that
Z3 then decides. The suite runs this over every file and requires zero reports.
On top of that, a differential fuzzer in `symbolic_test` (fixed seed) builds 400
random linear systems with `floordiv`/`ceildiv`/`mod` and requires:

- no answer that contradicts Z3;
- no false "final" claim;
- on systems with at most 3 variables and no division, every question Z3 decides
  is also decided locally.

Building the fuzzer found a real bug: the exact tier had treated an opaque
division atom from a polynomial normal form as a free variable while still
claiming to be complete.

### Z3 encoding

Facts become assertions over mathematical integers, and each question is a
validity check: `facts ∧ ¬q` unsat means `Yes`, `facts ∧ q` unsat means `No`.
`floordiv`/`mod` by a positive constant are SMT-LIB `div`/`mod`, which round
toward negative infinity for a positive divisor. `ceildiv(x, c)` is encoded as
`-((-x) div c)`. Atoms (`%7`, `tile`, `?%7.0`) become Int constants named by
their spelling. Every check runs on a fresh Z3 solver under a deterministic
resource limit, described in the Determinism section below.

`BRRR_SOLVER` chooses what runs after the local tiers:

| Value | Behavior |
|---|---|
| unset or `z3` | the local tiers, then Z3 when built |
| `local` | the local tiers only (complete on linear questions) |
| `check` | the local tiers and Z3 on every question; Z3's answer is used, and `solver check: ...` is printed to stderr on a Yes/No disagreement or an overclaimed settled Unknown |

`BRRR_SOLVER_STATS=1` prints the tier counts on exit (`solver tiers: 51
theorem, 24 local, 44 exact, 5 z3, ...`).

`brrr passes` reports the active engine, for example
`(solver: z3, z3 5.1.0, rlimit 6000000)`.
The test suite runs every example and case file in `check` mode and fails on
any disagreement. A disagreement means one of the engines is unsound, so the
sweep acts as the local rules' regression test.

## Determinism

An answer depends only on the facts and the question. Machine load and the
questions asked before it have no effect, so a recorded run replays exactly.
Three choices make this hold:

- **A resource limit, not a clock.** Each check gets Z3's `rlimit`, which counts
  internal work deterministically (default 6,000,000). The default was
  calibrated over the test suite and the examples. The most any check there
  consumes is about 570k: the degree-4 product `0 <= %s * %s * %b * %b` in
  `06_shapes.br`. Every other check costs a few hundred units. A check that runs
  out is an honest `Unknown`, and it is the same `Unknown` on every machine. A
  10 s wall clock remains as a safety net only. If a check hits it, its
  `Unknown` is not reproducible, so a warning is printed and
  `stats().safety_net_hits` counts it.
- **A fresh solver per check.** Facts are asserted in a canonical order:
  equalities, then inequalities, then ranges, then residues. Z3 therefore sees
  the same input whatever came before. The test suite checks this: the same
  question costs the same rlimit before and after twenty unrelated questions,
  and checking every file four times in parallel gives output identical to a
  sequential run. The seeds are fixed (`smt.random_seed`, `sat.random_seed`).
- **Recorded answers.** The run store keeps `answers.txt`, which maps each
  question's key (Z3 version, rlimit, facts, question) to its answer, under a
  128-bit hash. `run`, `resume` and `branch` replay a recorded answer instead
  of asking again:
  - a branch from `r1@0` with the same pipeline asks Z3 nothing and reaches a
    byte-identical state;
  - each stage record counts its answers (`solver 0 asked, 10 replayed`);
  - each run manifest names the solver (`solver z3 5.1.0 rlimit 6000000`);
  - `resume` under a different Z3 version or limit prints a note. Recorded
    answers still replay, but new questions may be answered differently.

  `check` mode never replays, because its job is to ask again.

| Setting | Effect |
|---|---|
| `BRRR_SOLVER_RLIMIT=N`, `--solver-rlimit=N` | per-check limit (0 = none) |
| `BRRR_SOLVER_STATS=1` | on exit: checks, the largest rlimit any check used, and checks stopped by the limit or by the safety net |
| `BRRR_SOLVER_STATS=2` | also print every check: its cost, its result, and the formula |

## Getting Z3

CMake option `BRRR_Z3` (default `ON`) looks for `z3.h` and the library
under `BRRR_Z3_ROOT`, which defaults to `third_party/z3`, and then in the
system paths. If nothing is found, the build continues without Z3 and the
local rules decide alone.

- Linux and macOS: install the system package (`libz3-dev`, `brew install z3`),
  or point `BRRR_Z3_ROOT` at a release.
- Windows (MinGW): unpack a release into `third_party/z3/{include,lib}`. GNU ld
  links `libz3.dll` directly without an import library, and the build copies
  the DLLs next to the binaries. `third_party/` is git-ignored, and
  `third_party/z3/SOURCE.txt` records where the copy came from. The MinGW build
  links its own runtime statically, so `libz3.dll` cannot pick up a mismatched
  `libstdc++`.

Z3 is MIT-licensed.

## Simplification

`symbolic::z3::simplify(e)` runs Z3's context-free `simplify` and translates
the result back into the expression language: numerals, constants, `+ - *`,
unary minus, and `div`/`mod` by a positive constant. It returns `nullptr` when
the result leaves the language. The `canonicalize` pass treats this output as a
**candidate** only: it keeps a rewrite when the result is smaller **and** the
solver proves it equal to the original under the facts in scope. A constant
pinned by the facts (`constantValue`) is preferred over any rewrite.

## What the solver does not do

- **Nonlinear facts.** Z3 accepts them, but nonlinear integer arithmetic is
  undecidable, so expect `Unknown` on hard queries. The local rules handle
  products only through the polynomial normal form and intervals.
- **`min`/`max`** (tier 3, deferred) and **division by a value** (never):
  integer division must say how it rounds, and dividing by a value needs
  non-zero and sign facts the language does not express.
- **Bit-vectors.** Expressions are mathematical integers. Wrap-around is kept
  out at the source: a value of type `iN`/`uN` has a meaning only when its op
  carries `nsw`/`nuw`, or when it is a widening cast (see `syntax-v1.md`,
  "Soundness").
