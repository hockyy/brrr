# src

The BridgeIR core library (planned — nothing scaffolded yet).

C++20, own IR — no MLIR (or LLVM) dependency in the core. Codegen and interop live at
the edges as adapters/subprocesses. Spec: `../docs/scratch/from-scratch-core.md`.

Planned layout:

- `ir/` — Context, Type/Attr, Op, Value, Block/Region; formats; verifiers
- `registry/` — data-first definitions registry (ops/types/attrs/patterns/checks)
- `pass/` — pass manager, pipeline-as-data, feedback journal
- `rewrite/` — pattern data model, matcher, fixpoint driver
- `plugins/` — plugin loader, version pinning
- `tools/` — `bridgeir` CLI + agent surface (JSON-RPC/MCP)
