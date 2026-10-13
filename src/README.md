# src

The BridgeIR core library. C++20, own IR — no MLIR or LLVM dependency in the core.
Codegen and interop live at the edges as adapters/subprocesses.
Spec: `../docs/scratch/from-scratch-core.md`.

## Status — prototype 5 (2026-10-13)

Implemented:

- **Dual identity**: stable machine ids + editable names; renames journaled.
- **Canonical text format** with round-trip guarantee; facts are part of the
  document (first-hand symbols: `fact N = 512`, `aligned`, `range`).
- **Schema registry**: kinds as data — named ports, results policy, typed attrs,
  inheritance (`kind relu < unary`), traits, and **declared primitives**
  (`scalar f16 { domain: float, bits: 16 }`; `dtype=@f8` is rejected). Every load
  validates declared vocabulary; foreign kinds are reported (`bridgeir check`,
  `--schemas=<file>`).
- **Symbolic layer**: equality / bounds / alignment queries, tri-state.
- **Connector hub**: SymPy escalation with caching and attribution.
- **Encodings**: canonical text (parse format), JSON (`to-json`), and a C-like
  view (`print --c-like`) with ids preserved as comments.
- **Provenance journal**; demo + fold-reshape passes.
- **Tests**: `ctest` suites (core, types, symbolic, connector) + 30 CLI checks.

Paradigm: **procedural core + object-shaped data** — passes/rules are procedures;
nodes are records with kinds; methods live in rules (data), not on objects.
See `../docs/scratch/paradigm.md`.

## Build & test

    cmake -S src -B src/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build src/build
    ctest --test-dir src/build --output-on-failure

## Walkthrough

    src/build/bridgeir print     src/tests/cases/add.bad.br    # normalizes to canonical
    src/build/bridgeir roundtrip src/tests/cases/add.canon.br  # ids survive reparse
    src/build/bridgeir demo      src/tests/cases/add.canon.br  # n6 minted, old ids kept
    src/build/bridgeir rename    src/tests/cases/add.canon.br sum total
    src/build/bridgeir check     src/examples/reshape_facts.br # vocabulary report
    src/build/bridgeir schema                                  # scalars + kinds
    src/build/bridgeir to-json   src/examples/reshape_facts.br # JSON encoding
    src/build/bridgeir print --c-like src/examples/reshape_facts.br  # mini-C++ view
    src/build/bridgeir fold-reshape src/examples/reshape_facts.br    # facts-driven

## Examples — external IRs mapped into BridgeIR

`src/examples/`: `matmul_hivm.br` (AscendNPU-IR HIVM), `fx_mlp.br` (torch.fx),
`dot_llvm.br` (clang LLVM IR CFG), `reshape_facts.br` (facts + fold-reshape).
Design analysis: `../docs/scratch/external-ir-mapping.md`.
