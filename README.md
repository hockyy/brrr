# BridgeIR

A multi-backend IR and compilation stack designed to be **operated by agents and extended at runtime**.

## Direction

- **IR substrate**: MLIR — out-of-tree dialects in C++, pinned to a fixed LLVM/MLIR commit.
- **Multi-backend**: each backend is a dialect + target interface + conversion pipeline, described by a manifest.
- **Plug and play**: pipelines, verifiers, and invariants live in data (manifests, scripts, schemas) so the stack can be reconfigured at runtime without rebuilding the core.
- **Agent-first**: extensions flow through a plan → build → load loop with structured diagnostics, per-pass IR dumps, and machine-readable pass/dialect descriptions.

Status: design phase.
