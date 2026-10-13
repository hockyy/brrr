# src

C++ sources for BridgeIR (planned — nothing scaffolded yet).

Planned layout (tentative):

- `dialects/` — core dialect(s) plus per-backend dialects (ODS/TableGen + C++)
- `tools/` — the `bridgeir-opt` driver: manifest registry, plugin loader, diagnostics
- `backends/` — target interfaces and conversion pipelines per backend

Substrate and pin: see `../docs/scratch/design-notes.md`.
