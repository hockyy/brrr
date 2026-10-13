# BridgeIR — working notes (scratch)

Non-paper material. Paper prose lives one level up (`main.tex` + `sections/`).
This file is the design/decision log; promote items into the paper as they stabilize.

## Goal

BridgeIR: a compiler + IR stack that is

- plug-and-play at runtime,
- operable/extensible by agents end to end — pipelines, pass definitions, verifiers,
  invariants, new op/type definitions,
- self-specializing: every compilation may mint fresh components, while staying fast
  enough to be SOTA on compile time.

## Requirements (as stated)

- Multi-backend IR.
- Pipeline "plug and play" at runtime.
- Run by agents in a good way.
- Easy to add: passes, definitions, verifiers, invariants.
- Refined: "not only pipelines, but everything" — agents get at the whole extension
  surface, not just pipeline composition.
- Refined: "everything is in house so we can easily code it" — full patch rights on
  the whole stack.
- Refined: own the library from scratch; new concepts allowed — "an IR made for
  agents", compact and easy to code. MLIR interop is a preference, not a pillar.

## Vision (Hocky, 2026-10-13)

> "Our goal here is to create from scratch a compiler that is friendly to agents. […]
> Our vision is to make everything extendable, fast, and loose. So that things like
> feedback + AI assisted compiling is the best here, but we keep it so in the future
> compiling a kernel doesn't need much rewrite of the compilers. Our goal is to build
> a compiler that is sustainable in the future for many kinds of kernel and frontend,
> and backend, many kinds of model, but it is fast and we don't recode everything for
> the beginning."

Normalized:

- **Extendable, fast, loose.** Narrow interfaces; components replaced/added at runtime.
- **Feedback-first.** AI-assisted compiling is the product; feedback quality decides.
- **Sustainable.** New kernel/model/frontend/backend ⇒ add components, don't rewrite.
- **Own the core, borrow at the edges.** The IR/framework is ours; heavyweight worlds
  plug in as adapters.

## Self-specializing compilation (per-compile synthesis)

> "So it's possible every time that we compile we create sets of new everything —
> including passes, pipelines, definitions, and features — yet the compiling part must
> be fast so that we are in the SOTA of compilation." (Hocky, 2026-10-13)

Law: `compile_time = reuse + generation + search/planning + application`.
Per-compile generation is only affordable via **staging + caching + budgets**:

- **S0 reuse** — pool hit, zero generation.
- **S1 pipeline plans** — pass order/selection from manifests + cost hints (ms).
- **S2 per-kernel artifacts** — schedules and rewrite sets (autotuning; e-graph rule
  inference); keyed by kernel class, cached.
- **S3 session/def specialization** — staged pass instantiation (partial evaluation
  w.r.t. types/config); schema-instantiated defs and verifiers.
- **S4 synthesized code** — superoptimized peepholes, generated matchers (ISLE-style);
  seconds–minutes; AOT/background/cached, never in the per-kernel hot path.

Corollaries:

- Content-addressed cache everywhere: `(IR fingerprint, config, target) → artifact`.
- **Search belongs to agent time, application to compile time.** Agents explore
  asynchronously; compile time only applies and verifies cached artifacts.
- **Provenance-based incrementality is required** (not optional): re-derive only
  invalidated artifacts. Provenance/feedback is the backbone layer, invariants the
  trust layer.
- **Budgets + fallbacks:** every generation stage gets a deadline and a cheap generic
  fallback; compile latency has a hard ceiling, quality scales with budget.

Precedents: Futamura projections / partial evaluation (theory); Truffle/Graal, PyPy
(fast per-program specialization in practice); TVM/Triton autotuning (per-kernel
schedules, production); Ruler (rule inference); superoptimizers (Bansal–Aiken, STOKE,
Souper); SPIRAL (generative compilation per problem); Peggy (component synthesis with
cost models); ISLE (declarative → compiled matchers). Caveat: new definitions + new
pass code per compile is the expensive end — v0 target: S1–S3 per compile, S4 cached.

## Extension tiers (T) — how a component gets in

- T0 pipelines as data (manifest → pass-pipeline strings). No rebuild.
- T1 interpreted transformation scripts (selector-based). No rebuild.
- T2 data-defined rewrite rule sets. No rebuild.
- T3 runtime definitions + verifiers from schemas (the primary definition path).
  No rebuild.
- T4 compiled plugins: new passes/backends as native or WASM plugins. Built by a build
  service (incremental; seconds), loaded at process start.
  "Reload" = restart worker, NOT in-process hot-swap.
- Escape hatch for untrusted agent code: WASM host behind a stable C ABI.

(Extension tiers T say *how* a component gets in; the S ladder says *when* it can be
minted per compilation. They compose: e.g. an S2 rewrite set enters via T2.)

## Architecture sketch (v0)

- Components (narrow interfaces + manifests): frontends/importers, IR + definitions,
  passes/rewrites, verifiers/invariant checkers, backends/emitters, feedback
  collectors (diagnostics, timings, costs, counters).
- Control plane (ours): registry, planner, runner, feedback bus (structured/JSON),
  plugin loader, build service (source → plugin), artifact cache (content-addressed).
- Agent surface: describe/list/plan/run/snapshot/feedback/submit; MCP-friendly.
- Substrate stance: **own core library from scratch** (decision 2026-10-13; spec in
  `from-scratch-core.md`). Concepts are ours — not MLIR-shaped. MLIR/StableHLO interop
  is an optional adapter and a preference at most, never a dependency.

## Environment facts (verified)

- `~/pg/llvm-project` @ `85081add2d8b` (2026-09-04), LLVM 24.0.0git, at
  `~/pg/llvm-project/build`. Config: Release+asserts, `LLVM_ENABLE_PROJECTS=mlir`,
  `LLVM_TARGETS_TO_BUILD=Native`, `BUILD_SHARED_LIBS=OFF` (static `.a` libs),
  `LLVM_ENABLE_RTTI=OFF`. Retained for bishengir/reference; no longer BridgeIR's
  substrate.
- System: Fedora 44, stock LLVM 22.1.8 (`llvm-devel` installed); `mlir`/`mlir-devel`
  available (~174/215 MiB installed). Useful for interop spelunking only.
- Toolchain facts relevant to any build: `clang`, `ninja`, `ccache`, `lld` present;
  176 cores; ~503 GB RAM.

## MLIR feasibility (verdicts, 2026-10-13 — reference record)

| Vision requirement | Verdict |
|---|---|
| Multi-backend lowering | Proven (Triton/IREE/CIRCT) |
| Agent-composed pipelines (S1) | Native, zero rebuild |
| Interpreted transforms (S2) | Native: transform dialect + PDL |
| Runtime defs + verifiers (T3) | Feasible, second-class (no runtime ODS) |
| Runtime passes (T4) | No interpretation: plugin + restart (compile .so/WASM) |
| Structured feedback / provenance | Build it (print-ir hooks + diagnostics are raw material) |
| SOTA compile speed | Engineering, not default |

Hard edges: (1) passes are compiled, not interpreted; (2) dynamic definitions are
second-class; (3) speed must be engineered. Kept as reference — the "bigger than
MLIR" ideas there remain borrowable.

## MLIR cost model + mitigations (measured 2026-10-13 — superseded by own-core pivot)

Measured on this box: build = **6.0 GB** (Release+asserts, `Native` targets, mlir-only);
`bin` = **3.4 GB** (146 tools, each statically carrying MLIR+LLVM; `mlir-opt` = 284 MB
raw / 212 MB stripped); `lib` = 1.3 GB (562 `.a`); headers ≈ 60 MB;
`mlir-opt --version` startup = **14 ms**.

Bloat autopsy: the bulk is *static duplication* (one copy of MLIR per tool), i.e. a
build-flag artifact — but the architectural objections (build-time ODS, C++ pass wall,
no provenance) stand independently. Kept as due-diligence record.

## Rival IR "forms" (candidate related work)

Full survey of families, matrix, lessons and gaps: `ir-landscape.md`.

E-graph (egg, egglog; TASO/Tensat) · relational/Datalog IR (SPORES, egglog) ·
schedule-first (Halide, TVM TensorIR, ISL schedule trees) · sea-of-nodes (Graal, C2) ·
formal semantics (Vellvm, CompCert) · demand-driven/incremental (Adapton, Salsa) ·
declarative selection (Cranelift ISLE — declarative authoring, compiled execution) ·
staged/generative compilation (Futamura projections, SPIRAL, Truffle/Graal).

## Playbook

Domain superset, never core replacement: Triton, Torch-MLIR, IREE, CIRCT are all
"bigger than MLIR for their domain" without forking the core. Forking the core =
years of work + lost ecosystem + chasing churn.

Note (2026-10-13 pivot): we are not forking MLIR — we own a *minimal* core with new
concepts and borrow at the edges. The superset logic applies to the ecosystem we plug
into (backends, frontends) via adapters.

## Open questions (paper-level)

- Defensible novelty claim? Candidates: "agent-operability as a first-class IR design
  axis" (now literal: an IR with new, agent-first concepts); "staged
  self-specialization with provenance + invariants"; feedback-first compilation.
  Must commit before writing the design section.
- Evaluation: agent operability (task success, time-to-first-extension, tokens,
  error recovery) + extension cost (time-to-add, files touched in core) + compile
  latency under generation. Baselines: MLIR/`mlir-opt` workflows vs BridgeIR.
- Core concepts: lock scope model (flat + scope edges vs nested groups), text syntax,
  selector grammar. See `from-scratch-core.md` §Open decisions.
- Core gaps demanded by external-IR mapping (named port groups, blocks/args/yields,
  ref-valued attrs, effect edges): see `external-ir-mapping.md` §6.
- Core language: C++20 (assumed; matches ecosystem, fast builds) vs Rust.
- v0 self-specialization scope: S1–S3 per compile, S4 cached? (proposed)
- Compile-latency budget + fallback policy per stage? (proposed: hard ceiling)
- What counts as "runtime"? Restart-based reload acceptable? (proposed: yes.)
- Paper title, venue, timeline.

### Candidate titles

- BridgeIR: An IR Made for Agents
- BridgeIR: An Agent-Native Compiler That Is Extendable, Fast, and Loose
- Compiling by Generation: Staged Self-Specialization for Agent-Assisted Compilers

## Decision log

- 2026-10-13: repo `hockyy/BridgeIR` created (private); local `~/pg/BridgeIR`.
- 2026-10-13: `docs/` = the paper (LaTeX); scratch = `docs/scratch/`; `src/` = code.
- 2026-10-13: vision recorded (extendable/fast/loose; feedback-first; sustainable;
  reuse-not-recode).
- 2026-10-13: self-specializing compilation adopted as design direction; staging law
  S0–S4; provenance promoted from "candidate differentiator" to required layer.
- 2026-10-13: MLIR cost audit on this box: build 6.0 GB, bin 3.4 GB (146 tools),
  `mlir-opt` 284 MB/212 MB stripped, startup 14 ms.
- 2026-10-13: **pivot — own core library from scratch.** Rationale: architectural
  (data-first definitions, provenance, no C++ pass wall) + measured weight. MLIR
  demoted to optional adapter.
- 2026-10-13: agent-first core vocabulary drafted (nodes/edges/types/data/schemas/
  deltas; selector + template rewrites; transactions). Concepts ours, not MLIR-shaped.
  Spec: `from-scratch-core.md`.
- 2026-10-13: dual identity adopted: stable machine ids (never renumbered) +
  human/agent-editable node names (unique per scope; renames journaled). Prototype:
  `bridgeir rename`, name-aware references and printing.
- 2026-10-13: symbols + typing design (`symbols-and-types.md`): all names are
  bindings (value vs symbol class); symbol refs become `Ref` data (`@name`,
  `@mod::@fn`); typing rules are schema data solved by unification. Prototype:
  `src/types/` + `ctest -R types` (dim propagation, failing-equation diagnostics,
  polymorphic ops, symbolic dims).
- 2026-10-13: symbolic-value layer designed (`symbolic-inference.md`): unknowns as
  data, facts as constraints, tri-state queries (equal / less / aligned / bounds).
  Prototype `src/symbolic/` + `ctest -R symbolic`. Contrast: in MLIR dims are SSA
  values (`tensor.dim`/`reifyResultShapes`) analyzed per-pass (ValueBounds/Presburger);
  here facts live in the IR and queries are a core service.
- 2026-10-13: connector layer (`connectors.md`): queries are data → any language can
  be a connector; local solver first, escalate on Unknown; SymPy connector
  implemented with caching/attribution/budget (`ctest -R connector`).
- 2026-10-13: grounded the motivating pass: bishengir `CanonicalizeTensorReshape.cpp`
  ("Without divisibility API, parting can only assume...", static-only
  `getStaticTotalMult`, `areLooseReassociationsCompatible` duplicated across 4
  passes). With facts, its decision = `isEqual` + `aligned` queries; Unknown maps to
  a runtime guard instead of an assumption.
- 2026-10-13: symbols/facts promoted to **first-hand language features**: `fact`
  lines in `.br` documents (equality / aligned / range), expression parser,
  consistency checks at apply time; `bridgeir facts` + `bridgeir fold-reshape`
  demo passes reading facts straight from the IR text. (User's point: the ability
  must be native to the language, not a bolted-on library.)
- 2026-10-13: schema registry v0 wired: kinds with named ports / results policy /
  typed attrs; validation on every load (`check`), foreign vocabulary warned;
  inheritance (`kind X < Parent`) + traits; `bridgeir schema|check`, `--schemas=`;
  examples converged (`ret` -> `func.return`, `load` -> `llvm.load`).
- 2026-10-13: paradigm recorded (`paradigm.md`): **procedural core + object-shaped
  data** (explicitly not Java-style OO: no method dispatch on objects, no class
  hierarchies — inheritance is flat field sharing); methods are rules/procedures as
  data. Encodings interchangeable — JSON wire format added (`to-json`); return
  types defined by typing signatures (`result_type`), to be carried in the schema
  next.
- 2026-10-13: primitive data types declared in the schema (`scalar f16 { domain:
  float bits: 16 }`, ...); attr type `dtype` validates against declared scalars
  (`dtype=@f8` is an error).
- 2026-10-13: mini-C++ surface engaged: C-like **view** implemented (`print
  --c-like`) — agents are C++-fluent, so a readable skin is high-leverage;
  semantics/ids/facts unchanged (ids as `// nID` comments); a parseable C-like
  dialect is the candidate third encoding. (`paradigm.md`)
- 2026-10-13: modules formalized as the top-level unit and symbol root:
  `module @name(mID) { func ... }` (kind `module` declared, validated); nested
  scopes print/round-trip; name resolution walks the scope chain; modules are the
  serialization/exchange boundary for connectors and importers.
- 2026-10-13: `#include` implemented (tool-level expansion, cycle-checked,
  `--include=<dir>` search paths): IR composes textually like C. Architecture
  statement recorded: thin **core runner** (main head + parser + runner/executor),
  and **the rest is just modules** — IR fragments via include, vocabulary via
  `--schemas`, engines via connectors, passes via plugins. Demo:
  `examples/app_includes.br`.
- 2026-10-13: control flow made first-hand (`control-flow.md`): **any node opened
  with `{` is a scope** (modules/funcs/blocks uniformly); declared
  `block`/`br`/`cbr`/`phi` (+ `yield` reserved, `icmp`/`arith.*` common vocabulary);
  CFG loop example `examples/loop_cfg.br`; `dot_llvm.br` rewritten to native block
  scopes. Rule: names are scope-local, stable ids are global — cross-block values
  use ids by design. Structured regions land on the same mechanism (region args +
  multi-port results outstanding).
- 2026-10-13: block arguments adopted as the canonical CFG form (phi keeps
  LLVM-fidelity imports): blocks take `param` args, branch delivery via `edge
  target passed*`, arity enforced schema-side through traits (`block`/`edge`/`arg`);
  `loop_cfg.br` rewritten to block-argument form (no cross-block ids anywhere);
  `bad_edge.br` covers the violation. Rationale: pass ergonomics, edge-site
  parallel-copy semantics (no lost-copy ambiguity), unified argument concept.
- 2026-10-13: steelman + alternatives recorded (`ssa-joins.md`): phi wins on
  IR-minimality, def-side analysis ergonomics, mechanical construction, and LLVM
  interop; block args win on human/agent ergonomics; the difference is where the
  parallel copy lives. Added `select` (predication route) to the control module.
  Conversions are vocabulary-level (`phi → args` normalization pass is next).
- 2026-10-13: verification argument added for edges over phi (`ssa-joins.md`):
  phi needs a derived predecessor index (invalidated by every CFG edit), pair
  parity/coverage, LLVM-style operand/successor order coupling, and dominance at
  edge ends; block arguments reduce the join to a call-site check. Implemented in
  the verifier: edge-use restriction (terminators only, exactly one), predecessor
  derivation, phi pair/coverage checks; negative tests `bad_edge_use.br`,
  `bad_phi.br`.
- 2026-10-13: raised the declaration level (user: phi is "declared in a very low
  level way that we cannot proof it"): schema ports are now typed (`target: block`,
  `passed: value*`) and support **port groups** (`(value, block)*`). The generic
  checker proves grouping and port kinds from the declaration and the hand-written
  phi parity/block checks were deleted; only the relational residue (edge arity,
  phi coverage) stays code — flagged as level-3 "constraints as data". Negative
  test `bad_phi_odd.br`; `schema` output shows the tuple declaration.
- 2026-10-13: simple structured loop added (user: "we just need a better way to
  represent loops"): `loop` is a scope whose `param`s are the carried values,
  `loop.yield (cond, next*)` terminates the body, `loop.N` ports are the exits.
  Relational checks: params == initial operands, exactly one yield, yield arity ==
  params+1. `loop_simple.br` (vs `loop_cfg.br`); negative `bad_loop.br`. Std/control
  port declarations typed; schema printer renders types and `( ... )*` groups.
- 2026-10-13: C++-shaped sugar (`for`/`while`/`if`/`else`) implemented as a textual
  lowering step (`src/ir/sugar.cpp`, runs before parsing): for/while → `loop` +
  params + `loop.yield` (while discovers carried names from body assignments);
  if/else → `cbr` + branch blocks + `edge`s + a join block carrying the node
  id/name. Honest limit: conditions/steps are SSA value names, not expressions —
  expression/assignment lowering = SSA construction, the next frontend level.
  Examples `loop_cpp.br`, `branch_cpp.br`; negatives `bad_for.br`, `bad_if.br`.
- 2026-10-13: type terms get their single parser (`types/type_parse.cpp`):
  one grammar (`name`, `name<arg,...>`, nested) for every declared type — no
  per-type parsers; declarations (`type tensor { params: dtype: scalar, shape:
  dims* }`) drive validation. Also fixed an enforcement hole the `>=` fact exposed:
  the expression lexer returned `End` instead of `Bad`, silently truncating
  `fact a >= b` to `a == b`; unknown operators now fail loudly and `>=`/`<=` are
  real inequalities.
- 2026-10-13: language showcase added (`src/showcase/`, 8 files + index, round-trip
  and check enforced by the suite): identity, facts/invariants, declared
  vocabulary, canonical CFG, structured loops, C++ sugar, modules + `#include`.
  `ask` gained functor queries (`aligned(n, 16)`) alongside comparisons. Tour
  index: `src/showcase/README.md`; design notes per feature in `docs/scratch/`.
- 2026-10-13: **decided (not yet implemented)** — two surface redesigns:
  (1) *identity*: an id is a C++-style identifier (`acc`, `i`, `sum`), document-
  unique, with no scope-local resolution; internal stable ids remain for
  provenance/caches, so renames never break identity. Names become identifiers and
  **visibility is dominance/sanctioned-roads, not name scoping** — the showcase's
  `unknown name 'e_then'` failure is exactly why (a child-scope `edge` cannot be
  named from the parent, yet the canonical CFG form requires it). Also kills the
  printer's resolve-back heuristic: a node prints the same everywhere.
  (2) *types*: types are **struct-like records**, not positional type constructors:
  declaration `type tensor { dtype: scalar; shape: dims }`, usage
  `tensor { dtype: f16, shape: [64, 128] }` — named, order-independent,
  self-describing, recursively validated by field declarations. Angle-bracket terms
  (`tensor<f16, 64>`) demote to the *interop* syntax (`parseTypeTerm`), not the
  native surface. Unifies attrs, ports, and types as one mechanism: declared
  records. The language then has exactly three declarables — `scalar` (nullary),
  `type` (record), `kind` (operation schema) — all data.
- 2026-10-13: **decided (not yet implemented)** — generic vs custom syntax, MLIR's
  two-form contract adopted, with the generic form *shorter* than MLIR's:
  (a) *short generic* is normative and requires declarations — no sigils/quoting,
  positional operands, attrs by name, results implicit (`id =`), no per-op type
  signature, declared defaults elided, `{ }` bodies with indentation;
  (b) *explicit generic* is the lossless fallback for undeclared/foreign kinds
  (MLIR-shaped: quoted kind, type signature, all attrs) so unknown vocabulary still
  round-trips; (c) custom per-kind syntax is *declared data* (a `syntax:` template
  over field names, parser+printer generated), and free-form surface grammars stay
  pre-lowering passes (`sugar.cpp`). Views, not modes baked into text:
  `print --explicit` / `--types` / `--generic` / `--c-like`. Budget rule: the
  generic form is the shortest lossless encoding *given declarations*; everything
  elided must be reconstructible from the schema.
- 2026-10-13: **decided (not yet implemented)** — vocabulary lives in *compiler
  modules*, never in documents. `scalar` / `type` / `kind` are defined in library
  files (`lib/core.bs`, `lib/types.bs`, `lib/ops.bs`, `lib/control.bs`, ...),
  versioned, loaded by the compiler as a default image (already the case: the
  core/std/control modules are compiler-side, and schema files are a separate
  format). Documents contain only data — nodes, facts, scopes — plus `import
  <module>` declarations resolved from the module search path (built-in +
  `--module-path=` / `--schemas=`), with content-hash pinning later. Inline
  vocabulary in a `.br` is now a clear error ("cannot be declared in a document;
  define it in a module and `import` it"); test `bad_vocab.br`. Types therefore
  arrive via `lib/types.bs` (struct-like records) alongside kinds.
- 2026-10-13: primitives refined into **families**, split into two kinds:
  (a) *element primitives* (declared, `lib/types.bs`): `bool`; `int{N}` /
  `uint{N}` (two's complement, N >= 2; the module may declare the supported width
  set); `float{bits, exp, mant}` — one parameterized family covering IEEE and
  bfloat/MX variants (float16 = 16/5/10, bfloat16 = 16/8/7, float32 = 32/8/23,
  float64 = 64/11/52, float8_e4m3 = 8/4/3, float8_e5m2 = 8/5/2) with the familiar
  spellings as module-declared names; `index` (word-sized, extents/offsets);
  (the `bool` domain is kept deliberately, not folded into `uint{1}`: predicate
  positions — `cbr`, `select`, `loop.yield` — must not accept arbitrary 1-bit
  integers; the distinction C++/Rust keep and LLVM/MLIR trade away with `i1`).
  Not primitives by design, they are *records*: tensor, ptr, complex, quant(elem,
  scale), resource/handle — and `token` too, as an empty record (`type token { }`)
  rather than a domain. No `void` — the results policy covers it.
  (b) *data (term) primitives* (metalanguage, not declared): arbitrary-precision
  `int`, wide `float`, `bool`, `string` (UTF-8), `bytes`, `symbol`/identifier,
  `ref` (stable id), `list`, `map`, `null`. Rationale: attribute payloads must not
  be bounded by element widths (a 128-bit mask literal is an attr `int`), and big
  payloads belong in side tables, not attrs.
- 2026-10-13: **decided (not yet implemented)** — extended/dynamic types are
  library-only and declared *class-shaped*: `class DynamicInt : int32 { ... }` in
  `lib/*.bs`. A class body is data, never methods-in-C++:

  ```
  class DynamicInt : int32 {          # extends a primitive / another type
    fields: { unit: string }          # extra fields (closed by default)
    open: true                        # extension point: later modules may add
    lower: int64                      # representation / lowering target
    checks: { value_in: range, mod_ok: aligned }   # invariant hooks, by name
    syntax: "dynint unit"             # custom syntax template for the parser gen
    traits: pure
    hooks: { verify: checker@v1, lower: plugin@ascend }   # engines, by reference
  }
  ```

  Execution still comes from the generic engines (checker, solver, parser
  generator, plugins) — a class *names* behavior, it never carries code.
  Two senses of "dynamic" kept distinct: (i) **link-time extension** — classes can
  be opened and extended by later modules, all resolution happening when the
  module image is linked, so documents stay statically typed at compile time
  (recommended); (ii) **runtime tagged values** — that is a *record/variant* type
  (`variant { tag: uint8, ... }`), never a dynamic core type system. Consequences:
  extension = schema growth at link time, so module version/hash pinning and
  visibility matter (a document must not depend on fields it did not import), and
  declaring `scalar`/`type`/`kind`/`class` inside a document stays forbidden.
- Substrate: own library (C++20, no heavy deps).
- Agent runtime target: T0–T3 without rebuild; T4 via plugins.

## Citation anchors (verify before submission)

See `../refs.bib`: MLIR CGO'21, egg POPL'21, egglog PLDI'23, SPORES POPL'22,
Alive2 PLDI'21, Vellvm POPL'12, TASO SOSP'19, TensorIR ASPLOS'23, Halide PLDI'13,
Adapton PLDI'14, Futamura 1971, SPIRAL Proc. IEEE'05, STOKE ASPLOS'13, Ruler OOPSLA'21
(to verify), TVM OSDI'18, Triton MAPL'19; Salsa / StableHLO / ISLE as `@misc`.
