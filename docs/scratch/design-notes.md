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
- Substrate: own library (C++20, no heavy deps).
- Agent runtime target: T0–T3 without rebuild; T4 via plugins.

## Citation anchors (verify before submission)

See `../refs.bib`: MLIR CGO'21, egg POPL'21, egglog PLDI'23, SPORES POPL'22,
Alive2 PLDI'21, Vellvm POPL'12, TASO SOSP'19, TensorIR ASPLOS'23, Halide PLDI'13,
Adapton PLDI'14, Futamura 1971, SPIRAL Proc. IEEE'05, STOKE ASPLOS'13, Ruler OOPSLA'21
(to verify), TVM OSDI'18, Triton MAPL'19; Salsa / StableHLO / ISLE as `@misc`.
