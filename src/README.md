# src

The BridgeIR core library. C++20, own IR — no MLIR or LLVM dependency in the core.
Codegen and interop live at the edges as adapters/subprocesses.
Spec: `../docs/scratch/from-scratch-core.md`.

## Status — prototype 1 (2026-10-13)

Implemented:

- **Stable node identity**: ids are allocated once, never renumbered; the text printer
  and parser preserve them exactly (`ir/ir.h`).
- **Canonical text format**, line-oriented, with round-trip guarantee
  (`ir/text.{h,cpp}`).
- **Provenance journal** sketch: every mutation records action/id/detail
  (`Module::log`).
- **Demo pass** (`passes/demo_fuse.*`) that mints exactly one fresh id while leaving
  every existing id untouched, and journals the insert + retarget.
- **Tool** `bridgeir`: `print | roundtrip | demo`.
- **Tests**: golden canonicalization, id-stable demo output, journal content,
  round-trip — via ctest.

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
