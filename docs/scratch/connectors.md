# Connectors: the IR talks to other languages and libraries

Status: design + first implementation (SymPy), 2026-10-13. Code: `src/connectors/`,
test `ctest -R connector`. Serves the "super loose" requirement: the core stays
small, and everything outside it is a connector behind a data protocol.

## The principle that makes interop trivial

**Queries are data.** An expression is a term (name + args over symbols); a query is
`{op, exprs, facts, budget}`; an answer is `{tri, source, justification}`. Anything
that can read and write text can be a connector — Python, Julia, Haskell, C, a
socket, a WebAssembly sandbox. No ABI coupling, no SSA values to thread, no C++
analysis classes.

Contrast: crossing an MLIR boundary means writing C++ analyses/interfaces and first
lowering the things you want to reason about into SSA values (dims via `tensor.dim`,
`reifyResultShapes`, `shape` dialect ops). Here, the *same* data that the local
solver consumes is what leaves the process.

## Protocol (v0)

```
query : { op: "isEqual" | "aligned" | "bounds", exprs: [text, ...],
          facts: [serialized], budget_ms }
answer: { tri: "Yes" | "No" | "Unknown", source, justification?, cost? }
```

Routing:

1. **Local first** — canonical form, intervals, residues (microseconds, in-process).
2. **Escalate on Unknown** — capability-matched connectors by priority (each declares
   what fragments it can decide: "nonlinear", "bitwise", "transcendental", ...).
3. **Unknown only if nothing decides.** Never guessed; the caller decides the policy
   (runtime guard, keep, defer).

Hygiene, all implemented for SymPy and mandatory for every connector:

- **Out of process** (subprocess now; RPC/WASM later), per-query deadline (`timeout`).
- **Content-addressed cache** — keyed by the fact-substituted expressions; repeated
  passes are hash lookups, not subprocess spawns.
- **Attribution** — every answer records `source` (`local` or `connector@sympy`), so
  provenance and paper claims stay honest.
- **Facts are applied before escalation** (`Solver::subst`), so the connector sees
  the strongest known form.

## Built-in: SymPy (implemented)

`src/connectors/` — local solver first; on `Unknown`, `connector@sympy` runs
`sympy.simplify(a - b).is_zero` in a budgeted subprocess and returns Yes/No/Unknown.
Transcript from `ctest -R connector`:

```
local:    (a+b)*2 == 2*a + 2*b              -> Yes  [local]         (no subprocess)
escalate: (a+b)*(a+b) == a*a + 2*a*b + b*b  -> Yes  [connector@sympy]
          second identical query            -> Yes  [connector@sympy] (cached)
escalate: a*a + 1 == a*a                    -> No   [connector@sympy]
escalate: a*b == a                          -> Unknown [connector@sympy]
```

**Query semantics** (worth being precise): `isEqual` is an *identity* question —
`Yes` means provably equal for **all** values, `No` means provably never equal.
`a*b == a` is **Unknown** because it holds at `a = 0`: satisfiable but not an
identity. Honest tri-state beats a wrong `No` — and a future `canEqual`/`exists`
query vocabulary can ask satisfiability questions explicitly.

The same subprocess pattern covers any language: Julia (`Symbolics.jl`), Mathematica,
Sage, Lean, or a custom decision procedure your team already trusts.

## Connector kinds beyond math

- **SMT (Z3/cvc5)**: nonlinear, bitwise, quantified queries — same tri-state API.
- **Reference evaluators**: execute the op on real data (numpy/torch) to check a
  rewrite's semantics; Yes/No plus a mismatch witness.
- **Emission connectors**: LLVM IR text / C source / NPU toolchain files — the
  "text edge" from `from-scratch-core.md` is just another connector.
- **Import connectors**: FX / MLIR-text / ONNX frontends (`external-ir-mapping.md`).
- **Agent connectors**: LLM agents that *suggest* rewrites, schedules, or fixes.
  They are untrusted by construction: suggestions enter as candidate data and must
  pass the solver, invariants, or a reference evaluator before any commit. This is
  how "AI-assisted compiling" participates without touching core trust.

## Why this is the right shape for Brrr

"Super loose" is not an add-on: it falls out of data-first. Connectors are plugins
with a manifest and a process boundary — the same extension model as passes and
schemas (T0–T4). A new language or library is *added*, never integrated: files
touched in the core = 0. And because answers are cached and attributed, the loose
edge never becomes nondeterminism inside the compiler.

## Status / next

- Implemented: SymPy escalation for `isEqual`, caching, attribution, budget.
- Next: Z3 connector; capability manifests; JSON query serialization (stdin/stdout)
  so connectors can live in any language without quoting tricks; checking external
  justifications (rewrite chains) when a connector can provide them.
