# BridgeIR core — v0 spec (from-scratch library)

Decision: **2026-10-13 — BridgeIR owns its IR/library, concepts included.** Rationale:
architecture (data-first definitions, agent-native, feedback/provenance) + measured
weight (MLIR build 6.0 GB; `bin` 3.4 GB = 146 tools statically carrying MLIR;
`mlir-opt` 284 MB). MLIR interop is a *preference*: an optional adapter, never a
dependency, never a concept model. The core must build in seconds and exist to serve
agents.

## Design criterion: an IR made for agents

The question is not "what does a compiler need?" but "what does an agent need?":

- **Learnability** — few concepts, regular shapes, cheap to read in tokens.
- **Addressability** — stable machine identities (ids never renumber) plus editable
  human/agent names (unique per scope, renames journaled); references survive steps
  and diffs stay clean (MLIR renumbers SSA values on every pass — hostile to agents).
- **Explainability** — provenance ("what changed, why, by which rule") is first-class.
- **Reversibility** — transactional edits with rollback; agents get try/undo for free.
- **Queryability** — one uniform way to find, inspect, and rewrite; no per-op APIs.
- **Budget** — timeouts, snapshots, incremental deltas; agent loops must stay fast.

## Core concepts (v0 draft — proposed vocabulary)

Concept budget target: ~6 primitives; everything else is data.

1. **Node** — the only operation concept:
   `{id, name, kind, ports, attrs, scope, loc}`. `id` is the stable machine identity
   (never renumbered); `name` is an optional, human/agent-editable label (unique per
   scope) whose changes are journaled deltas. `kind` names a schema; ports are ordered
   operands/results; there is no C++ class per op.
2. **Edge** — typed relation: `data` (value flow), `scope` (lexical parent),
   `effect` (ordering), `symbol` (name binding — every named node is a binding in
   its scope; late-bound symbol references are `Ref` data, `@mod::@fn`).
3. **Type** — a data term `{name, args}` (recursive) with unification variables;
   typing rules are schema data, solved by unification
   (see `symbols-and-types.md`). Not a class hierarchy.
4. **Data** — scalars/aggregates for attrs, templates, schemas: ints, floats, strings,
   refs, lists, maps — one serialization.
5. **Schema** — runtime definition of a kind/type/attr: ports, validation, rules,
   docs. Versioned, loaded from data, self-describing (schemas are IR data too).
6. **Delta** — every transform's output: adds/removes/mods + journal (rule, reason,
   diagnostics, timing). Transactions commit atomically; invariants checked at commit;
   failure reverts automatically.

Queries and rewrites share one **selector + template** language: the same grammar
selects nodes for inspection, for invariant checks, and for rewriting. No pass C++
classes needed for the common case; compiled plugins are an opt-in tier.

Sketch (concrete syntax TBD; shown only to feel the compactness):

```
func @matmul(n1) {
  n13 a_tile = load a
  n14 b_tile = load b
  n15 acc = mm a_tile b_tile type=f32<[M,N]>
  n16 act = relu acc
  n17 = ret act
}
```

Ids are forever; names are free. Renaming `acc` to `partial_sum` rewrites nothing
except the label — references follow because they resolve by name — and the journal
records `acc -> partial_sum`.

## What we keep from compiler tradition (don't relearn)

SSA-style dataflow, typed ports, verifiers, source locations, pass infrastructure,
versioned serialization. We re-shape the *framework layer* (definitions, rewriting,
provenance, transactions), not the semantics of SSA.

## What we deliberately drop or shrink (vs MLIR)

- Operation/Block/Region class hierarchies → nodes + explicit scope edges (open).
- ODS/TableGen build-time codegen → runtime schemas (data).
- SSA renumbering → stable IDs with a rename journal.
- Opaque passes → journaled, transactional steps with deltas.
- Whole-IR verification sweeps → invariant checks at transaction commits.
- Deeply nested text syntax → canonical line-oriented text, diff-stable, token-cheap.

## Scope

IN (ours, from scratch):

- IR: nodes/edges/ports/types/data; stable IDs + editable names; scope subgraphs;
  use-def lists.
- Registry: schemas as data (kinds/types/attrs/checks), runtime load, versioned.
- Symbolic layer: unknowns and expressions as data; facts (equality, ranges,
  alignment); tri-state solver queries with explanations.
- Connectors: external engines (SymPy, SMT, host-language libraries) behind a
  tri-state query protocol; cached, budgeted, attributed — the loose edge of the IR.
- Formats: canonical line-oriented text — symbols and `fact` lines are part of the
  document; versioned binary; content hash.
- Verifier framework: structural + semantic checks + per-commit invariants.
- Diagnostics: structured (code, severity, loc, payload), human + JSON.
- Pass manager: walkers, pipeline-as-data (JSON), budgets/timeouts, checkpoints.
- Query/rewrite engine: single selector + template language; fixpoint driver.
- Feedback journal: provenance (deltas, rule ids, reasons, timings) from day 1.
- Transactions: atomic commits, rollback on failed invariants.
- Plugin loader: dlopen, version/hash pinning, restart-based reload.
- Agent surface: CLI + JSON-RPC/MCP: describe/list/plan/run/snapshot/feedback/submit.

OUT (edges: adapters, subprocesses, plugins — never core):

- Codegen: v0 emits **LLVM IR text** and shells out to `clang`/`llc`; NPU emission via
  toolchain file adapters. Optional in-process LLVM plugin later, if ever.
- MLIR/StableHLO interop: import/export adapters, if wanted — preference, not a pillar.
- Autotuning/synthesis services (S2/S4): outside the core; artifacts enter via the
  content-addressed cache.

## Targets (the gates)

- Core library: clean build in seconds; binary < ~10 MB; no heavyweight deps.
- Agent loop: edit → build → load → run ≤ ~1 s.
- Extension cost: files touched in core = 0 for new kernels/defs/pipelines.
- MVP: parse → verify → 3 passes (one data-defined rewrite, one config-chosen
  schedule) → emit LLVM IR text → `clang` builds a tiny matmul → runs.

## Non-goals (v0)

- MLIR parity: no conversion framework; no ODS-style codegen (data-first = runtime
  descriptors; generate code only where measured).
- Own machine-code backend. The text-edge to LLVM/NPU tools is the strategy.
- Whole-model frontends: importers are adapters.

## Open decisions

- Scope model: flat graph + explicit scope edges (proposed) vs nested groups.
  Verifier contract needed either way: scope forms a tree; data uses respect scope.
- Text syntax: line-record style (proposed) vs s-expr; exact grammar.
- Selector language surface: s-expr vs SQL-ish; Datalog later maybe.
- Effect model: explicit effect edges only (proposed) vs ordered statements per scope.

## Risks / kill criteria

- Scope creep toward parity — the #1 killer of from-scratch IRs (Cranelift: funded
  years; QBE: ~10K LOC because it refused scope).
- Diagnostics quality (agents live on error messages) — budget it early.
- Pattern/selector engine design — borrow from PDL/ISLE literature, don't invent worse.
- Ecosystem isolation — mitigate with import/export adapters.

## Precedents

Cranelift (own IR + backend; funded, years, production — scope discipline), QBE
(~10K LOC C backend), Go/Zig (own IR, own backends), TVM (own IR + schedules),
sea-of-nodes IRs (flat graphs; Graal, C2). Plus the MLIR autopsy in `design-notes.md`.

## Milestones

1. Skeleton: Context/Node/Edge/Type/Data + text printer/parser round-trip.
2. Registry: data-defined kinds; verifier hooks; structured diagnostics.
3. Pass manager + pipeline JSON + transactions + feedback journal.
4. Query/rewrite engine + one data-defined rule set.
5. Emit LLVM IR text for a tiny matmul; `clang` builds and runs it.
6. Plugin loader + agent CLI; measure the ≤ 1 s loop.
