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

## Backends

Two engines sit behind the interface:

- **Z3** (optional, recommended). Facts become assertions over mathematical
  integers, and each question is a validity check: `facts ∧ ¬q` unsat means
  `Yes`, `facts ∧ q` unsat means `No`. `floordiv`/`mod` by a positive constant
  are SMT-LIB `div`/`mod`, which round toward negative infinity for a positive
  divisor. `ceildiv(x, c)` is encoded as `-((-x) div c)`. Atoms (`%7`, `tile`,
  `?%7.0`) become Int constants named by their spelling. Every check runs on a
  fresh Z3 solver under a deterministic resource limit, described in the
  Determinism section below.
- **Local rules** (always built). The polynomial normal form, an acyclic
  substitution from equality facts (orientation plus linear isolation), linear
  inequalities over the `<=` facts, interval evaluation with sound products,
  residues, and division bounds. This was the only engine before Z3. It still
  answers on its own when Z3 is absent, and it serves as the cross-check.

Before dispatching, `isEqual` runs a fast path: if the polynomial normal forms
of the two sides are identical, the answer is `Yes`; if they differ by a nonzero
constant, the answer is `No`. Both are theorems of integer arithmetic, so it
does not matter which engine would have answered.

`BRRR_SOLVER` chooses the engine at run time:

| Value | Behavior |
|---|---|
| unset or `z3` | Z3 when built, otherwise local |
| `local` | local rules only |
| `check` | ask both, return Z3's answer, and print `solver check: ...` to stderr whenever one says `Yes` and the other `No` |

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
