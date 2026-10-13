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
- **Reuse, don't recode.** Proven substrate stays; we own architecture + control plane.

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
  w.r.t. types/config); schema-instantiated op/type defs and verifiers.
- **S4 synthesized code** — superoptimized peepholes, generated matchers (ISLE-style);
  seconds–minutes; AOT/background/cached, never in the per-kernel hot path.

Corollaries:

- Content-addressed cache everywhere: `(IR fingerprint, config, target) → artifact`.
- **Search belongs to agent time, application to compile time.** Agents explore
  asynchronously; compile time only applies and verifies cached artifacts.
- **Provenance-based incrementality is required** (not optional): re-derive only
  invalidated artifacts. This settles the earlier deep-layer choice to (c)
  provenance/feedback, with (a) invariants as the trust layer.
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
- T1 transform-dialect scripts (interpreted transformations). No rebuild.
- T2 PDL rewrite sets (patterns as data). No rebuild.
- T3 runtime op/type defs + verifiers from schemas (ExtensibleDialect). No rebuild.
- T4 compiled plugins (.so): new passes/dialects/backends. Built by a build service
  (incremental; seconds on the 176-core box), loaded at process start.
  "Reload" = restart worker, NOT in-process hot-swap (MLIRContext is not reload-safe).
- Escape hatch for untrusted agent code: WASM pass host behind a stable C ABI inside an
  MLIR pass adapter.

(Extension tiers T say *how* a component gets in; the S ladder says *when* it can be
minted per compilation. They compose: e.g. an S2 rewrite set enters via T2.)

## Architecture sketch (v0)

- Components (narrow interfaces + manifests): frontends/importers, IR/dialect defs,
  passes/rewrites, verifiers/invariant checkers, backends/targets, feedback collectors
  (diagnostics, timings, costs, counters).
- Control plane (ours): registry, planner, runner, feedback bus (structured/JSON),
  plugin loader, build service (source → .so), artifact cache (content-addressed).
- Agent surface: describe/list/plan/run/snapshot/feedback/submit; MCP-friendly.
- Substrate stance: reuse MLIR behind a loose interface (in-house patch rights; swap
  path later). Under discussion — assumption open to correction.

## Environment facts (verified)

- `~/pg/llvm-project` @ `85081add2d8b` (2026-09-04), LLVM 24.0.0git, assertions build
  at `~/pg/llvm-project/build` (`mlir-opt` present). Pin candidate.
- `mlir-opt` supports: `--load-pass-plugin`, `--load-dialect-plugin`,
  `--mlir-print-ir-after[-all|-change|-failure]`, `--list-passes`,
  `--transform-interpreter`, `--pass-pipeline-file`.
- `mlir/IR/ExtensibleDialect.h`: runtime op definitions with `setVerifyFn`; dynamic
  traits with `verifyTrait`/`verifyRegionTrait`; `DynamicOpDefinition` carries
  `VerifyInvariantsFn`/`VerifyRegionInvariantsFn`.
  → runtime defs + runtime verifiers from data/schemas are feasible today.

## Rival IR "forms" (candidate related work)

E-graph (egg, egglog; TASO/Tensat) · relational/Datalog IR (SPORES, egglog) ·
schedule-first (Halide, TVM TensorIR, ISL schedule trees) · sea-of-nodes (Graal, C2) ·
formal semantics (Vellvm, CompCert) · demand-driven/incremental (Adapton, Salsa) ·
declarative selection (Cranelift ISLE — declarative authoring, compiled execution) ·
staged/generative compilation (Futamura projections, SPIRAL, Truffle/Graal).

## Playbook

Domain superset, never core replacement: Triton, Torch-MLIR, IREE, CIRCT are all
"bigger than MLIR for their domain" without forking the core. Forking the core =
years of work + lost ecosystem + chasing churn. Layers + one deep differentiator =
shippable, and that is the paper story.

## Open questions (paper-level)

- Defensible novelty claim? Candidates: "agent-operability as a first-class design
  axis"; "staged self-specialization with provenance + invariants"; feedback-first
  compilation. Must commit before writing the design section.
- Evaluation: agent operability (task success, time-to-first-extension, tokens,
  error recovery) + extension cost (time-to-add, files touched in core) + compile
  latency under generation. Baselines: vanilla out-of-tree `mlir-opt` flow vs tiers.
- v0 self-specialization scope: S1–S3 per compile, S4 cached? (proposed)
- Compile-latency budget + fallback policy per stage? (proposed: hard ceiling)
- What counts as "runtime"? Restart-based reload acceptable? (proposed: yes.)
- Plugin ABI for v0: internal .so vs stable C ABI vs WASM. (Proposed: .so first.)
- Paper title, venue, timeline.

### Candidate titles

- BridgeIR: An Agent-Native Compiler That Is Extendable, Fast, and Loose
- Compiling by Generation: Staged Self-Specialization for Agent-Assisted Compilers
- Feedback-First Compilation: A Self-Specializing Compiler Architecture

## Decision log

- 2026-10-13: repo `hockyy/BridgeIR` created (private); local `~/pg/BridgeIR`.
- 2026-10-13: `docs/` = the paper (LaTeX); scratch = `docs/scratch/`; `src/` = code.
- 2026-10-13: vision recorded (extendable/fast/loose; feedback-first; sustainable;
  reuse-not-recode).
- 2026-10-13: self-specializing compilation adopted as design direction; staging law
  S0–S4; provenance promoted from "candidate differentiator" to required layer.
- Substrate: MLIR (C++ out-of-tree), pinned to the LLVM 24 checkout above (tentative;
  patchable in-house).
- Agent runtime target: T0–T3 without rebuild; T4 via build service.

## Citation anchors (verify before submission)

See `../refs.bib`: MLIR CGO'21, egg POPL'21, egglog PLDI'23, SPORES POPL'22,
Alive2 PLDI'21, Vellvm POPL'12, TASO SOSP'19, TensorIR ASPLOS'23, Halide PLDI'13,
Adapton PLDI'14, Futamura 1971, SPIRAL Proc. IEEE'05, STOKE ASPLOS'13, Ruler OOPSLA'21
(to verify), TVM OSDI'18, Triton MAPL'19; Salsa / StableHLO / ISLE as `@misc`.
