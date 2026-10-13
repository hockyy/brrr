# src

The BridgeIR core library. C++20, own IR — no MLIR or LLVM dependency in the core.
Codegen and interop live at the edges as adapters/subprocesses.
Spec: `../docs/scratch/from-scratch-core.md`.

## Status — prototype 2 (2026-10-13)

Implemented:

- **Dual identity**: stable machine ids (`n17` — allocated once, never renumbered,
  preserved exactly by printer/parser) **plus editable names** (`acc`, `sum`,
  `a_tile`), unique per scope. Either form works as a reference in text, printing
  prefers the name when it resolves unambiguously, and every rename is a journaled
  delta.
- **Canonical text format**, line-oriented, with round-trip guarantee.
- **Provenance journal**: inserts, retargets, and renames all recorded.
- **Symbolic layer** (prototype): unknowns and expressions as data; equality,
  bounds, and alignment queries with tri-state answers (`src/symbolic/`,
  `ctest -R symbolic`).
- **Tool** `bridgeir`: `print | roundtrip | demo | rename`.
- **Tests**: canonicalization, id-stable demo, rename by id and by name, unknown-name
  rejection, journal content, round-trip — via ctest.

Not yet implemented: schemas/registry, verifiers, transactions, effect edges,
parser hardening.

## Build & test

    cmake -S src -B src/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build src/build
    ctest --test-dir src/build --output-on-failure

## Walkthrough

    src/build/bridgeir print     src/tests/cases/add.bad.br    # normalizes to canonical
    src/build/bridgeir roundtrip src/tests/cases/add.canon.br  # ids survive reparse
    src/build/bridgeir demo      src/tests/cases/add.canon.br  # n6 minted, old ids kept
    src/build/bridgeir rename    src/tests/cases/add.canon.br sum total  # by name!

Names are optional and human-editable; ids are forever. Renaming `n4 sum` to `total`
updates every printed reference automatically — and the journal remembers
`sum -> total`.

## Examples — external IRs mapped into BridgeIR

`src/examples/` holds hand-mapped external IRs (round-trip tested):

- `matmul_hivm.br` — HIVM-lowered matmul from AscendNPU-IR
  (`test/Conversion/HFusionToHIVM/matmul.mlir`)
- `fx_mlp.br` — a real `torch.fx` graph (torch 2.13)
- `dot_llvm.br` — `clang++ -O1 -emit-llvm` for a dot product, including CFG blocks
  and phis

Design analysis: `../docs/scratch/external-ir-mapping.md` (mapping frame, per-language
recipes, and the core gaps the exercise exposed).
