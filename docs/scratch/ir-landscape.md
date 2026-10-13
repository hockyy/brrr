# IR landscape — research notes (for BridgeIR)

Compiled 2026-10-13 from model knowledge (web search unavailable in this session).
Citation details marked **(verify)**. Purpose: (a) seed the paper's background/related
work, (b) extract design lessons for an IR made for agents.

Reading guide: families → compact entries (form, notable design choices, lesson for
BridgeIR). Then: cross-cutting matrix, ranked lessons, gap analysis, agent-era
systems, citation candidates.

A key theoretical fact up front: the major forms are *expressiveness-equivalent* —
SSA ⇄ CPS ⇄ ANF ⇄ sea-of-nodes can be translated into each other
**(verify: Kelsey'95 / Appel'98)**. So choosing a form is about *affordances*
(identity, verification, provenance, token cost), not about what can be expressed.
That is exactly the axis BridgeIR competes on.

---

## 1. Production language compilers (CFG + SSA lineage)

**LLVM IR** — SSA over basic blocks with φ nodes; typed values; module/function/block
hierarchy; text `.ll` + bitcode; dominance is the core invariant; verifier functions;
passes are C++ over generic walks; SelectionDAG below for ISel. Identity: values are
in-memory pointers; printed numbering (`%0`, `%1`) renumbers across passes.
→ *Lesson: dominance + typed ports are solved; stable identity is not.*

**GCC** — GENERIC (language-level trees) → GIMPLE (3-address SSA tuples) → RTL
(register-transfer lists). **(verify: Merrill, "Generic and GIMPLE", OSDI'03)**
→ *Lesson: staged lowering with different forms per stage — nobody keeps one form.*

**Clang / C++** — the Clang AST is itself an IR (rich, heavy; template instantiation);
CodeGen → LLVM IR. New: ClangIR/CIR, an MLIR dialect for C++ semantics (incubator).
→ *Lesson: language semantics (templates, UB) put irreducible weight somewhere; the
design choice is where.*

**Rust** — HIR → THIR → MIR → LLVM IR. MIR is a CFG of typed locals designed for
borrow-checking; it is **interpreted by Miri** (production IR-as-executable); used for
const-eval and niche analyses. *(verify: Miri paper/details)*
→ *Lesson: an IR designed for one analysis + an interpreter is enormously enabling.*

**Swift** — SIL: SSA with ARC/ownership as first-class ops; **staged canonicalization**
(raw → canonical), SILVerifier at each stage; then LLVM. *(verify: SIL docs)*
→ *Lesson: ownership/effects in the IR; staged canonicalization ≈ commit points.*

**Go** — cmd/compile SSA (values in blocks, machine lowering); simple by design,
fast to compile; good external tooling (`x/tools/ssa`).
→ *Lesson: simple IR + fast compile is a winning tradeoff.*

**C#/.NET** — Roslyn → CIL (stack-based, verifiable bytecode) → RyuJIT (flow graph,
SSA constructed during optimizing phases).
→ *Lesson: stack bytecode as portable form; JIT IR as phase-structured.*

**Java/JVM** — bytecode (stack machine, **bytecode verifier** = type-checking IR at
load time); C1 (fast tier: HIR→LIR) and C2 (**sea-of-nodes** ideal graph, GVN-style
fixpoints, scheduling at the end); Graal IR (sea-of-nodes with structured control).
→ *Lesson: two-tier IR strategy is production practice — maps to our S-ladder;
sea-of-nodes is a proven alternative to CFG+φ.*

**JavaScript engines** — V8: Ignition bytecode → Sparkplug → Maglev (mid-tier CFG IR)
→ TurboFan (graph IR with effect/control chains); SpiderMonkey: Ion/Warp MIR;
JavaScriptCore: DFG (SSA) → B3 (backend IR). *(verify: Maglev details)*
→ *Lesson: tiers multiply IRs by budget; per-compile component selection is normal.*

**LuaJIT / PyPy** — trace IRs: linear SSA over recorded execution paths; guards and
side exits; unrolling by construction.
→ *Lesson: IR derived from observation is maximally feedback-rich — the opposite
extreme from static IRs.*

**CPython** — bytecode + specializing adaptive interpreter (3.11+); 3.13 copy-and-patch
JIT. → *Lesson: runtime information can substitute for IR sophistication.*

---

## 2. Functional compilers

**GHC** — Core (typed lambda; System FC with coercions) → STG (closures/thunks; lazy
machine semantics) → Cmm (C--; explicit stack) → NCG/LLVM.
*(verify: "Implementing lazy functional languages on stock hardware", Peyton Jones'92)*
→ *Lesson: the evaluation model is encoded in the IR; typed core enables bold rewrites.*

**OCaml** — typedtree → Lambda (ANF-ish) → Flambda (inlining/optimization IR; closures
explicit) → Cmm → native (+ bytecode backend).
→ *Lesson: Flambda replaced the Closure IR *for inlining* — the IR is the optimizer's
interface; redesigning it is how you upgrade the optimizer.*

**Chez Scheme / nanopass framework** — a compiler as a long chain of *many tiny
languages*, each with an (auto-generated) verifier; passes derive from language
definitions. **(verify: Keep & Dybvig, nanopass, 2013)**
→ *Lesson: closest precedent to agent-authored tiny passes; we systematize it with
schemas + generated checks.*

**SML/NJ / FLINT** — CPS-based IRs; the CPS ↔ SSA correspondence (Kelsey'95; Appel'98
**(verify)**) ties this family to the CFG+φ family.
→ *Lesson: one node/edge graph can represent CPS and SSA at once — scope/effect edges
can carry continuation/block structure.*

**Erlang** — Core Erlang (readable functional core; typed-ish; optimization IR) →
BEAM bytecode (register VM, per-process isolation).
→ *Lesson: a human/agent-readable core for a dynamic language; bytecode as stable
target.*

**Chicken Scheme** — CPS → **C source**. → *Lesson: emitting portable source text as
the backend edge — same trick as our LLVM-IR-text plan.*

**Lean** — Lean 4: elaborates → IR → C (its own runtime). Proof assistants still
converge on imperative IR + C. → *Lesson: the C/text edge scales down and up.*

---

## 3. ML / tensor compilers

**XLA** — HLO: pure functional ops over shaped tensors; optimizations (fusion,
scheduling) as passes; lowers to LLVM. MHLO → **StableHLO**: a versioned, portable
subset — *stability as an explicit IR feature*. **(verify: StableHLO spec)**
→ *Lesson: functional graph IR; portability/stability must be negotiated explicitly.*

**PyTorch** — TorchScript (graph + interpreter + type system attempt); FX (a graph of
nodes **as a Python data structure**; tracer); torch.compile: Dynamo (bytecode → FX
graphs, with guards) → AOTAutograd → Inductor (lowers to loop-level IR; emits Triton
or C++). → *Lesson: IR-in-the-host-language is agent-friendly (inspectable,
printable); partial capture (guards, graph breaks) is acceptable.*

**JAX** — jaxpr: traced, functional, let-bound core; then HLO/StableHLO. jvp/vmap/grad
are *IR transforms*. → *Lesson: purity enables rewrites; tracing an execution builds
the IR.*

**Triton** — TTIR → TTGIR (**layouts/encodings as first-class types**: blocked,
shared, mma) → LLVM IR; Gluon (newer lower-level dialect with explicit control)
**(verify)**. → *Lesson: hardware layout encoded in types; performance decisions live
inside the IR; kernel-scale IR sits between graph IR and LLVM.*

**TVM** — Relay (functional graph IR; **op registration at compiler runtime** from
C++/Python; pattern matching); TensorIR (loop-nest IR: blocks, axes, annotations,
thread bindings; **schedules are transformation programs**: split/reorder/fuse/bind;
textual `tir-opt`); Meta-schedule/Ansor: search over schedule spaces.
**(verify: TensorIR ASPLOS'23 — in refs.bib)**
→ *Lesson: algorithm/schedule duality; schedules as data/programs — exactly our S2;
runtime op registration precedent.*

**Halide** — **algorithm vs schedule split**; scheduling language; loop IR with
storage annotations. → *Lesson: same algorithm × many schedules as first-class objects;
schedule authoring is a specialist act — a natural agent task.*

**ONNX** — **ops defined as data schemas** (versioned, type constraints, shape
inference); graph nodes reference values **by name**. **(verify: ONNX spec)**
→ *Lesson: the biggest "definitions are data" precedent — but with no transformation
engine and no provenance. That is precisely the other half BridgeIR builds.*

**IREE / Glow / nGraph** — deployment-oriented ML stacks (flow/hal/stream-style
dialects; latency/size budgets). → *Lesson: deployment constraints shape layering.*

**TASO / Tensat / e-graphs for tensors** — superoptimization by equality saturation +
cost extraction. **(verify: TASO SOSP'19 — in refs.bib)**
→ *Lesson: canonicalization as search over equivalences; complementary to rules.*

**In-house (Ascend)** — GE/AscendGraph (graph model + tensor formats/layouts), TIK/TBE
(kernel DSLs), BiShengIR/HIVM (MLIR-based Triton path), CANN binaries.
→ *Lesson: internal ecosystems carry their own graph IRs and file formats — our
emission edge must speak files first, handles later.*

---

## 4. Graphics / hardware / parallel IRs

**SPIR-V** — SSA + **mandatory structured control flow**; explicit memory model;
capability system; binary canonical + `spirv-val` validation; versioned + extensions.
**(verify: Khronos spec)** → *Lesson: structured control flow is a verifiability
feature; capabilities gate portability.*

**PTX** — textual virtual ISA (typed registers, SSA-ish), versioned; `ptxas` compiles
to SASS. → *Lesson: a stable *text* ISA as the compilation boundary — our LLVM-IR-text
edge is the same move.*

**WebAssembly** — structured stack-machine bytecode; **validation = load-time type
checking**; WAT text format. **(verify: PLDI'17 paper)** → *Lesson: safe-by-validation;
binary canonical, text for humans/agents.*

**eBPF** — register IR + a production **static verifier** that proves safety before
execution. → *Lesson: IR + verifier = trusted execution; the closest mainstream
analogue to our invariant gates.*

**SelectionDAG / GlobalISel** — DAG-based instruction-selection IR (legalization;
required transforms); GlobalISel is MLIR-based.
→ *Lesson: DAG form where selection needs it; even LLVM moved isel toward MLIR.*

**CIRCT / FIRRTL / Calyx** — hardware IRs (circuits, clocks, pipelines; non-software
semantics). → *Lesson: timing/hardware semantics belong in the IR when the target
requires it.*

**ISL / polyhedral / schedule trees** — IR as sets/maps + schedules; loop
transformations as (piecewise) affine maps. **(verify: Grosser et al.)**
→ *Lesson: a transformation *algebra* (schedule = data); mappings are provenance.*

---

## 5. Database / verification / relational

**Volcano / Cascades query optimizers** — logical plan → memo of equivalence groups →
rule-driven, cost-based search → physical plan; optimizers *remember derivations*
(which rules produced which expressions). **(verify: Graefe'93/'95)**
→ *Lesson: closest ancestor of plan + delta + provenance; borrow memo discipline.*

**Datalog analyses (Doop, Soufflé, polonius)** — program analyses as relational
queries. **(verify: Soufflé CAV'16)** → *Lesson: analyses-as-queries; our selector
layer can subsume this for simple checks.*

**Boogie / Why3 / Viper** — intermediate *verification* languages: procedural IR →
verification conditions → SMT. **(verify)** → *Lesson: IRs designed around their
checker; contracts as first-class citizens (our invariants).*

**egglog / SPORES** — Datalog ⇄ equality saturation; bottom-up rule evaluation with
incremental maintenance. **(already in refs.bib)** → *Lesson: rewriting as rules over
relations; incremental maintenance research to borrow from.*

---

## 6. Cross-cutting matrix (form × affordances)

| Form | Identity | Definitions | Verification | Provenance | Agent note |
|---|---|---|---|---|---|
| 3-address (GIMPLE, GIMPLE tuples) | implicit indices | C++ tables | pass-local | none | hard to reference |
| CFG + SSA (LLVM, MIR, SIL, JIT IRs) | renumbered per print | build-time (.td/ODS) | verifier fns | opt remarks (weak) | stable refs absent |
| Sea-of-nodes (C2, Graal, TurboFan) | pointers | C++ | strong structural | phase traces | graph shape learnable |
| Functional let-bound (Core, jaxpr, Relay) | binding names | runtime registry (TVM) or code | types | none | readable; refs lost on rewrite |
| Graph, name-keyed (ONNX, TF GraphDef) | **stable string names** | **schema registry (ONNX)** | shape/type checks | none | closest to agent needs |
| Loop + schedule (Halide, TVM TIR) | names | schedules as data | shape checks | implicit | schedules = agent artifacts |
| Structured bytecode (wasm, SPIR-V) | indices | validation tables | load-time type check | none | canonical + verifiable |
| Bytecode + VM (JVM, CIL, BEAM) | indices | tables | **bytecode verifier** | none | safe loading |
| Trace (LuaJIT, PyPy) | per-trace | guarded | guards | execution info | observation-driven |
| E-graph (egg, TASO) | **hash-consed, stable within session** | C++ (egglog: rules as data) | via rules | rule traces | equivalence classes as identity |
| Relational (Soufflé, egglog) | tuples | queries | constraints | lineage research | queries as interface |

---

## 7. Design lessons for BridgeIR (ranked)

1. **The multi-form chain is universal** — build a framework for chains (per-form
   schemas), not "one IR to rule them all". (GHC, GCC, Rust, Swift, TVM, V8)
2. **IR-as-executable is a superpower** — make the core interpretable from day 1
   (Miri, JVM/wasm interpreters): enables tests, differential checks, agent reasoning.
3. **Stable identity is the biggest miss** in optimizing IRs; only exchange graphs
   (ONNX, TF GraphDef) name things. Our stable IDs + rename journal is a genuine
   novelty, especially with deltas.
4. **Definitions-as-data works** (ONNX schemas, TVM runtime registry), but nobody
   pairs it with transformation + verification + provenance. BridgeIR = that pair.
5. **Verification precedent is strong** (wasm validation, eBPF verifier, SPIR-V,
   JVM bytecode verifier) — but all static/load-time. Ours: transactional commits +
   invariant gates at checkpoints.
6. **Schedule/data separation** (Halide, TVM) generalizes to our self-specialization
   (S2 artifacts as data).
7. **Layouts/encodings as types** (Triton) are required for real kernels.
8. **Many tiny verified IRs** (nanopass) is the philosophical ancestor of
   agent-authorable passes; systematize with schemas + generated checks.
9. **Search + provenance** (Cascades memo, derivation tracking) is the ancestor of
   per-compile planning with replayable derivation.
10. **Budget-tiered IRs** (V8/JVM tiers) generalize to the S-ladder.
11. **Text canon + binary speed** (WAT/wasm, .ll/bitcode, PTX) — copy this discipline.
12. **Stability must be negotiated** (StableHLO, SPIR-V capabilities, wasm proposals)
    → versioned schemas + feature gates.

---

## 8. Gap analysis — what no surveyed IR has

- Stable identity across optimization in an *optimizing* IR (only exchange graphs).
- Provenance/deltas as first-class values (DB optimizers closest; LLVM remarks weak).
- Definitions-as-data *combined* with transformations + verification + provenance.
- Agent affordances: token-lean canonical text, slices/views, stable refs, rollback.
- Budgeted, transactional transformation steps (JIT tiers budget, but aren't
  transactional; nothing gives agents try/undo).
- Any IR designed **as the interface for agent-driven compilation** — this is empty
  space. That is the paper.

## 9. Agent-era systems (knowledge-based — verify)

- KernelBench / TritonBench (LLM kernel-generation benchmarks, ~2025).
- AlphaEvolve (DeepMind, 2025): evolutionary agent improving kernels/algorithms —
  proves search + verification loops on real kernels.
- Meta LLM Compiler (2024): LLMs tuned for LLVM-IR tasks.
- ComPilot (2024): LLM-assisted compiler debugging / test generation.
- Compiler-R1 (2025): RL agentic compiler auto-tuning. **(verify details)**

Pattern: every system points agents at *human* IRs (LLVM IR, Triton, CUDA). None
designs the IR itself for agent consumption. BridgeIR is the first mover on the IR
axis, not the agent-tooling axis.

## 10. Citation candidates (verify before adding to refs.bib)

LLVM (Lattner, CGO'04); GIMPLE (Merrill, OSDI'03); GCC RTL; GHC/STG (Peyton Jones'92);
Kelsey'95 "CPS ↔ SSA"; Appel'98 "SSA is functional programming"; CPS (Flanagan et al.
'93 "The essence of compiling with continuations"); Nanopass (Keep & Dybvig'13);
MIR/Miri; SIL (Swift docs); Sea-of-nodes (Click'95); Graal (Würthinger et al.);
TurboFan; tensor compilers already in refs.bib (TVM, TensorIR, Triton, TASO, Halide);
ONNX spec; StableHLO spec; IREE; Halide; wasm (Haas et al. PLDI'17); SPIR-V spec;
eBPF verifier papers; Cascades (Graefe'95); Volcano (Graefe'93); Soufflé (CAV'16);
Boogie'05; Why3 (ESOP'13); Viper; Core Erlang; BEAM; Egglog/SPORES (in refs.bib);
AlphaEvolve/KernelBench/Meta-LLM-Compiler/ComPilot/Compiler-R1.

### Top-10 reads for our design

nanopass · ONNX schema registry · Rust MIR + Miri · Cascades provenance · Halide
schedule split · StableHLO (stability discipline) · wasm validation · TASO/egglog ·
V8 tiering · Kelsey/Appel (SSA↔CPS).
