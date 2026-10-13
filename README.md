# BridgeIR

An agent-native compiler: extendable, fast, and loose.

BridgeIR treats compilation as a feedback loop. Every artifact that drives a compile —
pipelines, passes, definitions, verifiers, invariants, schedules — is data that agents
can compose, generate, and load at runtime, and every compilation may specialize the
compiler to the problem at hand. The core stays small; components are added, the
compiler is never rewritten.

## Principles

1. **Agent-first feedback.** Structured diagnostics, per-stage IR snapshots, and
   cause→effect telemetry — built for automated agents to act on. AI-assisted
   compiling is the primary interface, not an afterthought.
2. **Extendable and loose.** Narrow interfaces plus manifests; components are
   discovered and loaded at runtime. A new kernel, model, frontend, or backend is
   added by adding components.
3. **Self-specializing, but staged.** Every compile may mint fresh pipelines, rewrite
   sets, schedules, and definitions — generated from specs, content-addressed and
   cached, with per-stage time budgets, so compile latency stays state-of-the-art.
   Search happens in agent time; application happens in compile time.
4. **Sustainable.** A new kernel class costs new schedules/rewrites as data and, at
   most, a new dialect. Files touched in the core: zero.
5. **Fast, on three clocks.** Time to extend, compile latency (agent loop latency is
   product latency), and generated-kernel performance.
6. **Own the core; borrow at the edges.** The IR, definitions registry, verifiers,
   passes, and feedback are ours, built from scratch — small, fast, data-first, no
   heavyweight dependencies. Codegen and other worlds (LLVM, NPU toolchains, MLIR
   ecosystems) plug in at the edges as replaceable adapters.

## Layout

| Path | Role |
|---|---|
| `src/` | the BridgeIR core library (planned; C++20, no heavy deps) |
| `docs/` | the paper (LaTeX — `docs/main.tex`) |
| `docs/scratch/` | working notes, decisions, design log |

Status: design phase.
