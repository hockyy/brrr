# Brrr — working notes (scratch)

Non-paper material. Paper prose lives one level up (`main.tex` + `sections/`).
This file is the design/decision log; promote items into the paper as they stabilize.

## Goal

Brrr: a compiler + IR stack that is

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
  `LLVM_ENABLE_RTTI=OFF`. Retained for bishengir/reference; no longer Brrr's
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
  latency under generation. Baselines: MLIR/`mlir-opt` workflows vs Brrr.
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

- Brrr: An IR Made for Agents
- Brrr: An Agent-Native Compiler That Is Extendable, Fast, and Loose
- Compiling by Generation: Staged Self-Specialization for Agent-Assisted Compilers

## Decision log

- 2026-10-13: repo created (private).
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
  `brrr rename`, name-aware references and printing.
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
  consistency checks at apply time; `brrr facts` + `brrr fold-reshape`
  demo passes reading facts straight from the IR text. (User's point: the ability
  must be native to the language, not a bolted-on library.)
- 2026-10-13: schema registry v0 wired: kinds with named ports / results policy /
  typed attrs; validation on every load (`check`), foreign vocabulary warned;
  inheritance (`kind X < Parent`) + traits; `brrr schema|check`, `--schemas=`;
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
  open class DynamicInt : int32 {     # `open` = later modules may add members
    int length;
    string unit = "t";                # default member initializer
  }
  # `type` uses the same C++-style member syntax, closed by default:
  #   type tensor { scalar dtype; dims shape; }
  # Behavior stays in sibling library blocks keyed by the name (never in members):
  #   syntax DynamicInt { "dynint length unit" }
  #   checks DynamicInt { value_in: range, mod_ok: aligned }
  #   lower  DynamicInt { int64 }              # representation / lowering target
  #   traits DynamicInt { pure }
  #   hooks  DynamicInt { verify: checker@v1, lower: plugin@ascend }
  # Member types: a primitive (`int32`, `float16`, `index`, `bool`), a declared
  # type/class (`tensor`, `ptr`), or a data primitive for metadata (`string`).
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
- 2026-10-13: **decided (not yet implemented)** — refinement of class-shaped
  types: *definition* may be **C++ code** (a registered implementation / plugin:
  semantics, verifier, lowering, optional custom parse/print), while the *surface*
  (members, syntax, traits, checks) stays data — either derived from the C++
  registration or restated in the library module that binds it
  (`impl: cpp@dynint`). Documents may **declare and initialize** class-typed
  values (`d = DynamicInt { length: 64, unit: "t" }`) and may declare
  *specializations* with bound members, but never behavior: a method/hook
  declaration in a `.br` is an error. Rule of thumb — **define = C++ or a library
  module; declare & initialize = document.** Pure-data classes (no `impl`) are
  plain records; that is exactly what `type` is, so nothing is lost for the
  no-C++ case.
- 2026-10-13: **decided (not yet implemented)** — *exports are per-piece*. A class
  registration exports its **shape** (members, base, syntax) and each semantic
  hook separately — verifier, lowering, checks, custom parse/print, cost model,
  conversions — every one a named, versioned unit. The shape declares which hooks
  it *requires* (`requires: verify, lower`); module linking must resolve each to
  exactly one registered implementation (version/hash pinned). A missing hook is a
  **load error** ("type exports shape but no verifier; link `verify@v1` or mark it
  `unchecked`"); duplicates conflict; resolution order is explicit. Unverifiable
  use is only allowed as an explicit, provenance-visible opt-in (`unchecked`),
  never a silent default — contrast MLIR, where an unimplemented interface
  soft-fails at query time. C++ analogy: the header exports the type, the `.so`
  exports the behavior, and the linker enforces that both are present.
- 2026-10-13: **decided (not yet implemented)** — `core.bs` is the **prelude**:
  implicit, unexported, unshadowable (primitives + the language skeleton kinds
  `module`/`func`/`param`/`func.return`). Only non-prelude modules (`lib/types.bs`,
  `lib/ops.bs`, `control`, `arith`, user libraries) form the export surface and
  need `import`. **Exporting/importing a module aliases it, never copies it**:
  identity is by origin (content hash), so `export core as lang`,
  `alias core as lang`, or a redundant `import core` are alternate *paths* to the
  same declarations — `lang.float16 ≡ float16` for unification and checking, with
  no new type identity and no duplicate-definition errors. Redefining a prelude
  name is an error; different semantics means declaring a *new* class with a new
  name, never shadowing `float16`. Aliases are also document-legal
  (`alias f16 = float16`) because they define no vocabulary — which doubles as
  token compression for agents.
- 2026-10-13: **decided (not yet implemented)** — the prelude's source of truth
  is **C++**: the internal core types (primitives + language skeleton) are defined
  beforehand and registered by the compiler at startup, so the default resolution
  of `float16` / `int32` / `index` / `bool` requires no `import` and no `.bs` file,
  and `brrr schema` reflects them from the registration. A `core.bs` may be
  generated for reference; if a module exports or aliases it, that is an **alias to
  the internal set** (same identity), never a redefinition. Consequence: `.bs`
  modules declare *extensions* — records, classes, extra kinds, hook bindings —
  while the internal names are reserved (alias yes, shadow no). A class whose
  semantics are C++ registers from code; a class with no C++ implementation is
  declared in a module as data. Same registry, same identity-by-origin, either way.
- 2026-10-13: **decided (not yet implemented)** — a document's **beginning is its
  header**: `import` declarations followed by **local data types** (records;
  data-only, no hooks, unexported, not importable, and cannot shadow prelude or
  module names), then the body (facts + nodes). Canonical printing always emits the
  header first, so diffs stay deterministic. This clarifies the earlier "no
  vocabulary in documents" rule rather than contradicting it: definitions *with
  semantics* live in C++ (prelude) or modules (exported), while a *local data
  record* used by a single document is allowed at the top, because it defines no
  shared vocabulary and carries no behavior. Error text updated accordingly:
  "declare it in the document header (local data type) or define it in a module
  and `import` it". (If the ask also meant C89-style "declarations first inside
  every scope", that is a printer preference only — the IR stays a flat node list,
  so it needs no parser support.)
- 2026-10-13: **decided (not yet implemented)** — declarations are C++-shaped and
  the **type goes on the left**: `int64 z = arith.constant value=0`,
  `int64 total, int64 last = for (int64 acc = z, int64 i = n; more;
  acc = na, i = ni) { ... }`. Written types are optional (inferred) but
  **verified**: the binder unifies written vs inferred type and reports the failing
  equation on mismatch. A multi-result node returns a **record value whose fields
  are the declared result names**, so results are read as `sum.total` (C++ member
  access) rather than anonymous `.0`/`.1` indices; single-result nodes keep the
  bare name. This unifies three ideas under one: results = ports = struct fields —
  a value is a scalar or a record with named fields. Consequences: the loop's
  carried set is a field set; sugar headers may carry optional type annotations
  (`int acc = z`) which lowering strips; and short spellings like `int` require a
  document alias (`alias int = int64`) because the prelude keeps explicit widths
  only.
- 2026-10-13: **decided (not yet implemented)** — MLIR's region model adopted
  uniformly: **any operation may carry a body, and a body always holds block
  arguments** (possibly none). Consequences: the `param` kind is **deleted** —
  function parameters are the entry body's arguments, `loop` carried values are the
  body's arguments, and a join block's parameters are its arguments. Arguments are
  declared in the scope header, typed (optional but verified), each with an
  identifier:

  ```
  func sum_to_n(int64 n) { ... }
  loop sum(int64 acc, int64 i) = (z, n) { ... }
  block join(int64 res) { ... }
  ```

  So "scope signatures" are no longer optional; they are the single canonical way
  arguments exist. `edge` and loop-init arity checks become a comparison against
  the target's declared argument count (today: scanning children with the `arg`
  trait), and the checker's per-kind special-casing disappears. Core delta is
  small: `has_body` gains an argument list. Mapping kept for reference: MLIR region
  ↔ our `body`; block argument ↔ header argument; successor operands ↔ `edge`;
  `yield` ↔ results (record fields); `RegionBranchOpInterface` ↔ our scoping +
  dominance rules over the same edges.
- 2026-10-13: **decided (not yet implemented)** — no step-assignment list in the
  header (`acc = na, i = ni` is *not* allowed in canonical IR). Carry-in is declared
  in the header, carry-over is stated by the body's `yield`, and the arity
  invariant is **three-way**: `carry-in bindings == body arguments == yielded
  values == result bindings`. Header form: `for (int64 acc = z, int64 i = n; more)
  { ... yield na ni }` — init in the header, the continue-condition as a named
  value (computable in the body, since header args are in scope and resolution is
  order-independent), updates only via `yield`. Therefore `loop.yield` is *pure*
  carry-over (`operands: value*`; the condition no longer occupies the first
  operand) and the check becomes "yield count == arg count" instead of "args + 1".
  Deliberate asymmetry: **sugar may infer carry-over, the IR never does** — C++
  `while (cond) { ... i = next ... }` keeps working because the preprocessor
  discovers assigned names and lowers them into explicit carry-in + `yield`; the
  canonical form is always explicit about what is carried and what is returned.
- 2026-10-13: **decided (not yet implemented)** — `do_while` is *not* a new
  construct: it is the same bodied-scope shape as a loop, differing only in **when
  the condition is evaluated**. So `for` / `while` / `do_while` are surface
  spellings over one `loop` kind carrying two attribute bits — condition position
  (`pre` for `for`/`while`, `post` for `do_while`) and polarity (`while` vs
  `until`) — while the header, carry-in bindings, `yield` carry-over, LHS result
  bindings and the three-way arity invariant are all identical to the loop rule
  above:

  ```
  int32 another_scope_like = do_while (int32 i = 1, int32 j = 3; until_condition) {
    ...
    yield i j
  }
  ```

  Consequences: no new checker path (same arity/scoping rules; `post` adds only
  "the body executes at least once"); no new sugar target (the preprocessor maps
  the keyword to the two bits and the printer emits the matching surface keyword,
  so round-trip is lossless); and one fewer MLIR carry-over — `scf.while`'s
  two-region shape is unnecessary because the condition is just a named value in
  the single body plus a position bit.
- 2026-10-13: **decided (not yet implemented)** — **drop `while`**. The surface loop
  vocabulary is just `for` (condition tested *before* the body) and `do_while`
  (tested *after*). So the one `loop` kind keeps a single structural bit,
  `cond_pos: pre|post`; the header's condition operand is always the **continue**
  condition (true → repeat), and polarity is spelling/naming, never structure — an
  `until` reading, if wanted, is sugar for the negation of that value. A C++
  `while (cond) { ... x = ... }` still works at the surface: it lowers to
  `for (x = init; cond)` with the assigned names hoisted into the header by the
  preprocessor. Net: three surface keywords become two, and the two-bit mode
  becomes one bit.
- 2026-10-13: **recorded + verified (implemented)** — `scf.while` → `br`/`cbr`
  lowering in the canonical block-argument form, worked end to end in
  `src/examples/scf_while_cfg.br` (`check`: 0 errors / 0 warnings; round-trip ids
  stable; guarded by the suite). Mapping: the `before` region becomes the loop
  header block whose **block argument is the iter_arg**; `scf.condition(%cond)
  %arg0` becomes **one `cbr` with two outgoing edges, both carrying `%arg0`** (true
  → `after`, false → `exit`); `scf.yield %next` in the `after` region is not a
  result yield but the **back edge** `br before(next)`; and the while's result is
  simply whatever the false edge carried. No `phi`: block arguments are canonical
  and edges deliver them. The invariant is the loop invariant (back-edge payload ==
  header args == forward-edge payloads), and both halves are enforced — verified by
  deliberate breakage: `passes 2 value(s) to block 'before' which takes 1
  parameter(s)` and `operand n16 ... is in block 'after' which does not dominate
  this use`. Consequence for coupling: our structured `loop` kind *is* `scf.while`
  with the two regions fused and `cond_pos` deciding where the test sits, so a
  `scf.while` can be imported either structured or as this cycle — the cycle is the
  semantics, the structured form is the optimization surface.
- 2026-10-13: **decided + implemented** — `phi` is **removed** from the language
  (schema, checker, tests, examples). A block's arguments, fed by its incoming
  `edge` values, are the *only* way a value crosses a block boundary. The
  "phi -> predecessor values" sanctioned crossing and the whole phi relational check
  (predecessor membership, duplicate from-block, coverage) are deleted, and
  `src/examples/dot_llvm.br` — clang `-O1` output, the exact case phi existed for —
  is remapped with edges + block arguments at `errors: 0`. Consequences: the
  checker's sanctioned roads are down to `terminator -> block/edge` and `edge -> its
  target`; no from-block bookkeeping survives anywhere, so "duplicate pair /
  coverage kept in sync by hand" is structurally impossible. phi was the one
  construct that made block arguments *optional*; now they are mandatory, which
  forces the block header to carry its arguments:

  ```
  after = block (int64 a, int64 b) { ... }     # target of `edge after x y`
  ```

  That header form is the target surface, but it lands **with the identifiers-as-
  names item**, not before: every value currently prints an `nID`, so a header
  written today would have to spell `(int64 n9 arg0)` — id *and* name, a
  three-token form the next migration deletes. A block argument's identity is
  (scope, position) and references are by name, so the header becomes `(int64 acc)`
  exactly when printed ids stop being the surface. Recorded so the two land
  together.
- 2026-10-13: **decided + implemented** — scope headers carry their arguments, and
  **an argument is an id: there is no name alias**. One declaration, in the header:

  ```
  n9 join = block (int64 n10, int64 n11) { ... }     # target of `edge join x y`
  n5 sum  = loop  (n6, n7) zero n2 { ... }           # carried args, then inits
  ```

  `param idx=k` lines are no longer needed for blocks/loops (still accepted as
  legacy input, normalized away on print). The parser rejects a name in an argument
  list (`block (int64 n7 acc)` -> "argument list entries are 'TYPE ID'"), because a
  name would be an alias and aliases are gone. The printer never prints a `param`'s
  name and never references one by name: every reference to an argument is by id.
  Consequences: examples/showcase sources were normalized (arg names dropped,
  references *and facts* rewritten to ids — e.g. loop_cfg `fact 0 <= n12`), queries
  name ids (`ask loop_cfg.br count "n7 <= n2"`), and the goldens moved to the
  id-only print. Function parameters still use the `param idx=` line form (funcs
  have no header argument list yet) but are ids too — their names are equally dead.
  This is the interim shape: the identifiers-as-names migration makes the printed
  ids *be* the C++ names, so the surface reads `block (int64 acc)` again — with
  `acc` as the actual identifier rather than an alias beside it.
- 2026-10-14: **v1 language** (`syntax-v1.md`, now the single source of truth for the
  surface). Review verdict on v0: decisions were being recorded faster than code
  used them (phi added then removed, `param` "deleted" but still everywhere, ~20
  concepts against a budget of 6), ids were hand-written, values were untyped and
  attributes stringly. v1: MLIR-familiar sigils (`%v`, `^b`, `@f`), types on values,
  successor arguments on the branch (no `edge` nodes), header arguments (no `param`
  nodes). Ids stay stable but leave the default view: `print` is clean, `print --ids`
  is the stored form, and edited clean text gets its ids back by name matching
  (`reconcileIds`). Removed: edge/param nodes, C++ sugar, the C-like view, the
  `dims`/`dtype`/`expr`/`sym` attribute types. The 25 v0 tests and all examples
  were rewritten; the old scratch notes below describe v0 syntax.
- 2026-10-14: **expressions as values.** Dims, facts, and integer operands are
  expressions whose atoms are SSA values, so binding a symbol is defining a value
  and SSA scoping is symbol scoping. Kinds declare their meaning (`expr: lhs - rhs`)
  and shape rules (`shape: [M, K], [K, N] -> [M, N]`) as schema data. An operand
  is a value, an expression, or a list of expressions — this replaces MLIR's
  `OpFoldResult`, static/dynamic operand pairs, and `kDynamic`. Attributes keep only
  configuration. `?` in a type is a fresh dimension with identity (`dim(%x, k)`);
  the solver got a polynomial normal form, an acyclic substitution (v0's `a = b`
  facts ping-ponged), and residual equalities.
- 2026-10-14: **soundness.** Signedness lives on the type (`iN`/`uN`/`index`), not
  the op; typed atoms carry their range; a value has a meaning only if it cannot
  wrap (`index` always, `iN`/`uN` with `nsw`/`nuw`). Facts got roles: module =
  assumption, function args = precondition, block args = invariant checked on every
  incoming edge with branch conditions (loops by induction), op results = lemma.
  Unknown equations are warnings, errors under `--strict`, or runtime `guard`s via
  `insert-guards`. Fixed in passing: v0 refuted `fact 0 <= n` whenever `n` was known
  positive (it tested the wrong direction).
- 2026-10-14: **tier-2 expressions + the affine bridge.** `floordiv`/`ceildiv`/`mod`
  by positive constants (MLIR's quasi-affine fragment); no `/`, because integer
  division must say how it rounds. Exact when divisible, else an opaque term with
  its defining bounds. `apply` = an expression as a value. `to-affine` /
  `from-affine` convert exactly; export picks each operand's `OpFoldResult` side
  (static / value / `affine.apply`) from the solver, and splits dims from symbols by
  SSA position. Tier 3 (`min`/`max`) deferred; division by values never.
- 2026-10-14: **runs.** A compile is a recorded history: runs have ids, every stage
  stores before/after states (content-addressed), any stage can be shown, branched
  from, or resumed with an edited `pipeline.txt` or a hand-edited IR (`--state`, ids
  reconciled). Each stage must verify and round-trip through text or it is rolled
  back. Pass registry with names and summaries (`brrr passes`).
- 2026-10-14: core and portability: `Module::find` is an id->index map (was a linear
  scan), journaled `replaceAllUses`/`erase`/`setType`/`rename`; MinGW builds link the
  runtime statically, output is LF on Windows, the SymPy connector no longer needs a
  POSIX shell. All four ctest suites pass on Windows.
- 2026-10-14: **Z3 behind the solver interface** (`solver.md`). Hand-written
  derivation does not scale: the local rules kept needing soundness fixes (half-bounded
  interval products, overflowed corners). Z3 now answers every question as a
  validity check over mathematical integers, with a 200 ms timeout that maps to
  Unknown. The local rules stay as the fallback when Z3 is not built and as a
  cross-check: `BRRR_SOLVER=check` asks both and reports Yes/No disagreements,
  and the suite runs that sweep over every file. Z3 is optional at build time
  (`BRRR_Z3`, `third_party/z3`); the core has no hard dependency on it.
- 2026-10-14: **a pass is one C++ file.** `passes/pass.h` is the whole pass API
  (`Context`, `PassInfo`, `BRRR_PASS`). The build globs `passes/*.cpp` into an
  object library, so adding a pass touches no list. The core (`brrr_core`: IR,
  parser/printer, schema, solver, sema) is the only exposed API; passes and the CLI
  are clients of it. First solver-backed pass: `canonicalize`, which rewrites every
  integer expression (operands, list elements, result dims) to a constant pinned
  by the facts or to Z3's simplification. A rewrite is kept only when it is smaller
  and proven equal, so Z3's simplifier proposes and the solver checks.
- 2026-10-14: **codegen to LLVM IR text** (#18; see `codegen.md`). The target is LLVM IR,
  not assembly: clang is the backend, and Brrr's proofs map onto LLVM's hints. Buffers
  are shaped types (`buf<[dims], e>`). `load`/`store` take one index expression per dim,
  and every index must be proven in bounds or it becomes a runtime guard. Proven code
  compiles to bare `getelementptr inbounds`. Facts now flow to dominated blocks (SSA,
  by induction for loops), so nested loops prove their accesses without restating
  outer facts. First binaries: dot, matvec and sum_to_n, run against expected values.
- 2026-10-14: **the name is Brrr** ("Bot-Readable, Reasoned, Reproducible"; compiler
  go brrr). Agents can read it, the solver backs its claims, and runs are recorded
  and replayable. Files are `.br`. The C++ namespace, the binary, the CMake targets
  and the `BRRR_*` variables all use the name, and runs live in `.brrr/`. The repo is
  public at github.com/hockyy/brrr.
- Substrate: own library (C++20, no heavy deps).
- Agent runtime target: T0–T3 without rebuild; T4 via plugins.

## Citation anchors (verify before submission)

See `../refs.bib`: MLIR CGO'21, egg POPL'21, egglog PLDI'23, SPORES POPL'22,
Alive2 PLDI'21, Vellvm POPL'12, TASO SOSP'19, TensorIR ASPLOS'23, Halide PLDI'13,
Adapton PLDI'14, Futamura 1971, SPIRAL Proc. IEEE'05, STOKE ASPLOS'13, Ruler OOPSLA'21
(to verify), TVM OSDI'18, Triton MAPL'19; Salsa / StableHLO / ISLE as `@misc`.
