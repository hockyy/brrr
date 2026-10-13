# SSA joins: phi vs block arguments — and the alternatives

Status: design note, 2026-10-13. Companion to `control-flow.md` (mechanism and the
block-argument form BridgeIR ships as canonical). This note steelmans phi nodes,
then surveys every alternative for representing joins.

## The dichotomy

A join must express: "the value here depends on which predecessor we came from."
There are exactly two placements:

- **Def-centric (phi):** all inputs co-located at the definition, in the join block.
- **Edge-centric (block arguments):** inputs live at the branch sites; the join
  block declares parameters.

The information is identical. The difference is *where the parallel copy lives*.

## Where phi nodes are genuinely superior

1. **Minimal instruction surface.** Branch ops need only successor labels. Block
   arguments require successor-operand segmentation (MLIR: `BranchOpInterface` +
   `getSuccessorOperands`; BridgeIR: an `edge` kind, plus an arity checker and a
   negative test we had to write). Fewer invariants for the core.
2. **Def-side analysis ergonomics.** "Who feeds this value?" is answered at the
   definition; def-use chains are complete on the value itself. With block
   arguments, gathering a join's inputs means visiting every branch targeting that
   block (or maintaining a parallel use-list keyed by edges). GVN, value numbering,
   and SCCP are def-centric.
3. **Mechanical construction.** Classic SSA construction (Cytron et al.) computes
   dominance frontiers and *inserts phis at join blocks* — the CFG is untouched.
   The edge form rewrites every predecessor terminator instead; more of the IR
   moves during construction.
4. **Uniform value class.** A phi is an ordinary instruction. Block arguments are a
   distinct Value subclass with argument-specific APIs that every pass must learn.
   The "skip phis when iterating" annoyance trades against "special-case block
   arguments" annoyance — it moves, it does not disappear.
5. **Local join edits.** Control-flow threading/splitting edits predecessors;
   phi-update rules stay local to the join block. Edge operands must be kept
   positionally aligned per successor — an invariant we had to encode in the
   verifier (`bad_edge.br`).
6. **LLVM lingua franca.** Bitcode, clang, Alive2, and most adjacent tooling assume
   phis; text interchange is 1:1 (`dot_llvm.br` maps clang output with zero
   normalization).
7. **Parallel copies have one home.** Out-of-SSA translation places copies at block
   entry. Block arguments relocate the same parallel copies to edges — they
   reappear when bottoming out at machine moves. The "lost copy"/swap bugs are
   properties of out-of-SSA *algorithms* (fixed by Boissinot et al., 2008), not of
   phi semantics.
8. **No arity coupling.** A phi's meaning never depends on a target block's
   parameter list; terminator and target can evolve independently.

## Where block arguments win

Pass iteration without phi special-casing; the branch site states what it passes
("what is looped back" is visible at the edge that loops it); unified argument
concept (function inputs and block inputs are both `param`); parameters are
block-local names, so no cross-block references. Human/agent ergonomics — not
expressiveness.

## Alternatives to both

| Form | Join representation | Used by |
|---|---|---|
| CPS / continuation args | join = jump to a continuation; values passed as call args | SML/NJ FLINT; SSA ≡ CPS (Kelsey'95, Appel'98) |
| Structured control flow | no arbitrary joins: carried values declared on the op (`iter_args` + `yield`) | MLIR `scf`, HLO `while`/`conditional` |
| Sea of nodes / control-as-value | merge node with projections; no basic blocks at all | HotSpot C2, Graal, TurboFan |
| Predication / if-conversion | joins become `select`; control at the join disappears | SIMD/vector ISAs; our new `select` kind |
| Memory-based joins (pre-SSA) | no join values: store to a slot, reload after | GIMPLE before mem2reg; naive frontends |
| Pure functional dataflow | joins illegal by construction (single assignment only) | HLO, jaxpr |
| Dependency/edge graphs | control implicit in the graph | dataflow machines; DFG IRs |
| E-graph / relational | "join" = congruence merge of equal terms | egg/egglog (optimization-time only) |

Conversions between the two main forms are standard: MLIR (block args) lowers to
LLVM (phi); the reverse — phi → args — is a classic normalization.

## Verification: the decisive argument for edges

Phi's invariant exists *because of the representation*:

1. **Predecessors are not stored.** They must be derived from terminators — a
   global inverted index that every CFG edit invalidates.
2. **Two-node mutual invariant.** Each `(value, from-block)` pair must correspond
   to a real edge, so the phi and the terminator's successor set must agree.
3. **Order coupling (LLVM).** Phi operand order tracks successor order; reordering
   successors silently invalidates phis unless both sites are updated.
4. **Dominance at edge ends.** The incoming value must dominate the *predecessor
   block's terminator*, not the use — a whole-function analysis run with special
   phi handling.
5. **Fiddly coverage rules.** Unreachable predecessors, critical edges, and
   self-loops all complicate pair coverage.

Block arguments reduce the same question to a **call-site check**: arity, types,
and the ordinary "operands dominate their use" rule. No derived index, no
cross-node parity, no ordering coupling — the join verifies the way function calls
verify.

Implemented in `verify/check.cpp` to make the asymmetry concrete: edge arity and
edge-use restrictions are purely local; the phi check needs `predecessors_of()`
(derived from terminators and edge targets) plus pair-parity and coverage
bookkeeping. Negative cases: `bad_edge.br`, `bad_edge_use.br`, `bad_phi.br`.

### Declaration levels (how far can a property be *stated*?)

| Level | Mechanism | Provable by |
|---|---|---|
| 0 | doc-comment conventions ("operands are pairs") | nobody |
| 1 | traits + hand-written checks | code review |
| 2 | typed ports and port groups (**now**) | the schema, generically |
| 3 | declarative constraints over the IR (next) | one generic engine (+ a solver) |

At level 2, `phi { operands: (value, block)* }` makes even grouping and
from-block typing *structural facts the schema proves* — the hand-written parity
and block checks were deleted. `edge { target: block, passed: value* }` proves the
target typing. What remains in C++ is exactly the **relational residue**
(cross-node: edge arity vs target parameters; phi coverage vs predecessors) — which
is level 3's job: state them as constraints, check (or discharge) them with one
engine instead of per-kind code.

## BridgeIR stance

Both are declared kinds in the `control` module. Canonical form: block arguments
(`edge` + `param`), because agents and passes read edges better. `phi` remains for
LLVM-fidelity imports. `select` covers the predication route. A `phi → args`
normalization pass is the natural next step; nothing in the core changes for any
of these — it is all vocabulary, swappable per module.
