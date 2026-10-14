# Symbols and math-like type inference

> **2026-10-14:** written for the v0 text format (`nN name = kind ...`, edge and
> param nodes). The current language is [`syntax-v1.md`](syntax-v1.md); the
> reasoning here still stands where v1 does not say otherwise.

Status: design + prototype step 1, 2026-10-13. Companion to
`external-ir-mapping.md` (which flagged ref-valued data and first-class types as gaps
#3/#4). Prototype lives in `src/types/` with a test (`ctest -R types`).

## Part A — Symbols: names with binding semantics

Everything named in Brrr is a **binding in a scope**. We already have the raw
material: per-scope node names (unique, editable, rename-journaled). Two binding
classes fall out naturally:

| | Value binding | Symbol binding |
|---|---|---|
| What | intermediate results (`acc`, `sum`) | entities: `func`, `global`, `type`, `block`, `backend`, `rule` |
| Reference | lexical `name` in operands (today) | `@name`, hierarchical `@module::@func::@block` (planned) |
| Binding time | definition order irrelevant, but value-flow | late-bound; visible in the whole scope subtree |
| Rename | journaled rename (done) | journaled rename + symbol-use list for O(uses) updates and DCE |
| Visibility | scope-local | `private` (default) / `public` (exported) / `nested` |
| MLIR analogue | SSA values | `SymbolTable` + `SymbolRefAttr` + symbol DCE |

Design decisions:

1. **One namespace of names; schema decides which kinds define symbols.** A kind
   marked `symbol` in its schema (e.g. `func`, future `global`, `type`, `rule`)
   registers a symbol binding in its scope when it has a name. Non-symbol kinds keep
   plain value-binding names. No parallel "attribute string" universe — MLIR's
   `SymbolRefAttr` becomes our **`Ref` data kind** (text: `@name`, `@mod::@fn`).
2. **Refs store both**: resolved stable node id (fast, survives edits) *and* the
   original path (re-emit, diagnostics, cross-file). Same trick as our name refs.
3. **Renaming symbols is the rename we already built.** Because references resolve by
   name at print time, `rename n4 sum -> total` fixes all uses; the journal records
   history. A symbol-use index makes it O(1) per use instead of a scan.
4. **Why this matters for pipelines and provenance**: rules, backends, and targets
   become symbols, so plan data and journal entries can cite them by name
   (`reason=rule:fuse_muladd`, `backend:@ascend::@aic`) instead of opaque pointers.
5. **Interaction with types**: named (nominal) types are symbols too — a type term may
   reference a type symbol (`@shape::@tile_t`) the same way ops reference funcs.

## Part B — Types as data, inference as math

The goal: adding an op's typing should feel like writing one judgment, and inference
should be automatic.

**The judgment is the schema entry.** For `TensorIR`-style ops that is literally one
line, stored as data:

```
mm  : tensor<A, M, K> x tensor<A, K, N> -> tensor<f32, M, N>      # A = dtype, M/K/N dims
add : T x T -> T                                                   # polymorphic elementwise
```

That is a `Signature` record: kind, type parameters, per-port type terms, result type.
Because type terms are `{name, args}` data with **unification variables**, the
"dtype A" and "dims M,N,K" are just variables.

**Solving is Robinson unification** (the math part, and it is tiny): instantiate the
signature per node with fresh variables, then unify operand types against the
signature's port types, occurs-checking; the instantiated result type is the node's
type. Consequences, all free:

- symbolic shapes: `tensor<f16, A, 128> x tensor<f16, 128, B> -> tensor<f32, A, B>`
- dimension agreement is enforced by unification, with the *failing equation* reported:
  `at tensor[1]: cannot unify 100 with 128`
- dtype polymorphism via `T x T -> T`; mixed dtypes fail with the same clarity
- typed IR as an **invariant**: run inference at transaction commits; a commit that
  cannot be typed is rejected, with the equation as the diagnostic.

**Layered constraints** (later tiers, same machinery):
broadcast/subsume (`shape(bias) ⊑ [B,128]`), divisibility/alignment (`N % 16 == 0`),
ranges — these are not equality, so they go to a second solver (pattern rewriting
first, SMT later). Equality-unification covers dims, dtypes, and polymorphism — the
part that must be effortless.

**Derived overlay, not canonical text.** Inferred types are *derived facts* keyed by
stable node ids: they are not written into canonical text (keeps round-trip
idempotent), and they can come with justification (the equations that produced them)
— provenance-native typings. A `brrr types <file>` view prints them on demand.

**Why this stays agent-friendly**: adding typing for a new op = adding one data
record; mistakes produce human-readable equations, not template errors; and the
inference results carry justifications that agents can cite.

## Part C — Worked example (FX graph + mm)

Given `w : tensor<f16, 64, 128>` (from `get_attr`) and `x : tensor<f16, B, 64>`:

```
mm(x, w) -> f32<B, 128>        # K unified from both operands; A unified to f16
add(mm, bias) : bias ⊑ f32<B,128>   # broadcast — tier-2 constraint, not equality
relu(...) -> same type
```

The first line is pure unification; the second shows the constraint tier boundary.

## Part D — Prototype status

Implemented now (`src/types/`, ~250 lines):

- `TypeTerm` (name + args + unification vars) as data; structural equality.
- `Unifier`: `fresh`, `unify` (occurs check, path-aware errors), `resolve`,
  `pretty` (`tensor<f32, 64, B>`).
- `infer(nodes, signatures)`: schema-as-data signatures, per-node instantiation,
  equation transcript; returns resolved per-node types or the failing equation.
- Test `ctest -R types`: dim propagation `(f16,64,128)x(f16,128,256) -> f32<64,256>`;
  mismatch diagnostic naming dim `K` (`at tensor[1]: cannot unify 100 with 128`);
  polymorphic `add`; symbolic dims surviving as variables.

Next steps:

1. Type terms in the text format (a `Type` data kind instead of `type="..."` strings)
   and a `brrr types` command over `.br` files.
2. Signature *files* (schema registry) so adding typing = adding data, not code.
3. Symbol refs (`@name`, `@mod::@fn`) in the parser; symbol-use index; visibility.
4. Tier-2 constraints (broadcast, alignment) as a rewriting pass; SMT only if needed.
