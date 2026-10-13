# The symbolic layer: value reasoning as a core service

Status: design + prototype step 1, 2026-10-13. Prototype: `src/symbolic/`,
test `ctest -R symbolic`. Refines `symbols-and-types.md` (which covered name
bindings and type unification).

## What "symbol dialect" means here

A **symbol** is an unknown value — `M`, `N`, `i`, `stride` — and expressions over
symbols are ordinary IR data: `N + 32`, `2*i`, `4*i + 1`. Symbols are not SSA values
that flow through edges; they are **data inside types and attributes**, declared in
scopes, constrained by facts, and reasoned about by a solver.

The core service this buys — the thing the user asked for — is:

> Given the facts in the IR, can we prove that two symbolic expressions have the
> same value? Prove an inequality? Prove 16-byte alignment? What are the bounds?

Answers are **tri-state** (`Yes` / `No` / `Unknown`). `Unknown` is a first-class,
honest result; nothing is guessed. Prototype transcript:

```
(a+b)*2 == 2*a + 2*b                      -> Yes
M = N + 32, N = 128  =>  M == 160         -> Yes     (bounds(M) -> [160, 160])
a == b (unconstrained)                    -> Unknown
a + 1 == a                                -> No
i < N   (N = 1024, i in [0, 1023])        -> Yes
i < 5                                     -> Unknown
aligned(M, 16)  with N = 0 (mod 16), ...  -> Yes     (M inherits from N)
aligned(4*i, 4)                           -> Yes     (no knowledge of i needed)
aligned(2*i + 1, 2)                       -> No
```

## How it works (and why it is small)

Three mathematical ingredients, each tiny:

1. **Linear canonical form.** Expressions normalize to `c + Σ coeff·var`, so
   `(a+b)*2` and `2a+2b` are literally the same term. Equality = canonical compare,
   with a subtlety: a nonzero *constant* difference proves `No`; a difference that
   contains unknowns stays `Unknown`.
2. **Interval evaluation** with fact-supplied ranges, so `i < N` follows from
   `i ∈ [0, 1023]`, `N = 1024`.
3. **Modular residue propagation.** Facts like `N ≡ 0 (mod 16)` propagate through
   `+` and `*`: `M = N + 32` inherits it; `4*i` is `0 (mod 4)` for any `i`.

Facts are asserted as data — by schema rules, importers, or agents:
equalities (`assertEqual`), ranges (`assertRange`), residues (`assertMod`).

## Why this is heavy in MLIR (the honest comparison)

In MLIR, dynamic dims are **runtime values**: `tensor<?x?xf32>`, extracted with
`tensor.dim`; result shapes are materialized back into SSA with
`reifyResultShapes`; the `shape` dialect builds computations over them. The analysis
machinery exists — `ValueBoundsOpInterface` for dims, Presburger (`IntegerRelation`,
`isDivisibleBy`, etc.) for affine facts — but it is *infrastructure you invoke from
each pass*, over *values you must first thread into SSA form*. There is no
place in the IR where symbolic facts live, no query service that answers "are these
two dims equal?", and nothing returns a justification. Doing it costs C++ per pass,
per analysis, duplicated across transformations — and by the time a dim is needed
for reasoning, it usually has to be reified, passed around, and re-derived.

(So: not impossible in MLIR — the primitives exist — but expensive, distributed, and
not first-class, which matches the user's intuition that this is heavy engineering
in the tensor-type layer.)

In BridgeIR the same facts are **data**: expressions live in types/attrs, facts are
asserted by schema rules, and queries are one solver call. No SSA threading, no
per-pass re-derivation.

## Where facts come from

- **Schema rules** (data): `mm` implies dim equalities (already handled by type
  unification); `split(x, 128)` yields `dim = N/128` symbolic shapes; an
  `assert`/`guarantee` node kind can carry explicit facts (`aligned(N, 16)`).
- **Importers**: FX / MLIR / HIVM adapters record what the source IR knew
  (`origin=` annotations).
- **Agents**: facts are the natural negotiation currency for schedules — S2
  artifacts declare "give me N aligned to 16" as data, checked by the solver.

## Integration with the rest of the core

- **Verification / invariants**: vectorized ops can *demand* `aligned(stride, w)`;
  a commit fails with the failed query as the diagnostic. Same machinery as
  branches of the transactional-commit story.
- **Type unification** strengthens: dims that are not syntactically equal but
  canonicalize to the same expression unify (`M*2` vs `M+M`) — no C++ needed where
  MLIR passes would hand-roll it.
- **Provenance**: a `Yes` can cite the facts used (equalities, ranges, residues);
  derived facts are keyed by stable node ids like inferred types.
- **Autotuning / legality**: schedule candidates ask the solver for legality
  predicates instead of hardcoding checks.

## Limits (honest)

- Fragment: affine integer arithmetic + modular facts. Nonlinear and bitwise
  reasoning → later tier (rewriting or SMT plugin); `Unknown` until then.
- `floordiv`/`mod` syntax in expressions is deferred; alignment is modeled via
  facts, not syntax, in v0.
- Growth: fact sets need scoping/invalidations — the journal + incremental
  machinery later keeps this cheap.

## Pass usage: ComposeReshapeLoosely / FoldReshape (grounded)

Real evidence from `bishengir/lib/Dialect/Tensor/Transforms/CanonicalizeTensorReshape.cpp`:

- `FoldReshape::isInferrable` gives up when dynamic-dim counts differ, and its
  comment admits the core problem: *"Only support 1 to 1 and/or 0 to 0 for now,
  other case should be derivable by logic! Without divisibility API, parting can
  only assume the value is the same."*
- The pass hand-rolls `RankedTensorWithInfo::getStaticTotalMult()` — the product of
  the **static dims only** — and compares those products; dynamic dims are assumed
  equal because there is nothing to query.
- `areLooseReassociationsCompatible` is re-checked with local variations in at least
  four passes (CanonicalizeTensorReshape, ComposeMultiReduce, SwapCollapseExpand,
  SwapMemrefCollapseExpand), each doing its own shape archaeology.

With the symbolic layer the same decision is two first-hand queries:

```
Tri same = facts.isEqual(sizeExpr(src), sizeExpr(dst));  // dynamic dims included
Tri ok   = facts.aligned(sizeExpr(dst), 16);             // vectorization legality
// Yes -> compose; No -> keep; Unknown -> conservative (runtime guard), never assume
```

No static-only products, no "assume the same", no def-use walk: facts are recorded
once (schema rules / contracts / importers) and the query is a solver call.
Prototype: scenario 6 in `src/tests/symbolic_test.cpp`.

## Escalation: connectors (SymPy, SMT, your language's libraries)

The local solver decides the affine fragment; everything else escalates through
**connectors** — out-of-process engines behind the same tri-state API, budgeted,
cached by content, and attributed. First connector implemented: SymPy
(`src/connectors/`, `ctest -R connector`). Design:
[`connectors.md`](connectors.md).

## Next steps

1. Facts in the text format (`fact ...` lines) and a `bridgeir decide`/pass CLI demo.
2. Schema-driven fact generation (`split`, `mm`, `pad`, `reshape` rules as data).
3. Justifications on local answers (`Yes because: N ≡ 0 (mod 16), 32 ≡ 0 (mod 16)`).
4. More connectors (Z3/SMT, host-language reference evaluators) behind the same
   tri-state API; capability manifests; answer checking.
