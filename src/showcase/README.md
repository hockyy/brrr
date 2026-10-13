# BridgeIR showcase — a tour of the language

Every file here round-trips (`bridgeir roundtrip`) and validates cleanly
(`bridgeir check`: 0 errors) in the test suite. Run from the repo root after
building (`cmake --build src/build`).

| File | Shows | Try |
|---|---|---|
| `01_identity.br` | stable ids, editable names, journaled renames, facts | `src/build/bridgeir print src/showcase/01_identity.br` · `... rename src/showcase/01_identity.br sum total` |
| `02_invariants.br` | root + scoped facts, tri-state queries | `... ask src/showcase/02_invariants.br count "i <= n"` → **Yes**; `... ask ... root "0 <= i"` → **Unknown** |
| `03_vocabulary.br` | declared ports/attrs/scalars, dims validation | `... schema` · `... check src/showcase/03_vocabulary.br` |
| `04_control_cfg.br` | canonical control flow: blocks, `edge`s, join params, dominance | `... ask src/showcase/04_control_cfg.br then "a >= b"` → **Yes** |
| `05_control_structured.br` | structured loops: carried params, `loop.yield`, exit ports (`sum.1`) | `... ask src/showcase/05_control_structured.br sum "i <= n"` → **Yes** |
| `06_cpp_sugar.br` | C++-shaped `for` / `while` / `if-else`, lowered on parse | `... print src/showcase/06_cpp_sugar.br` (see the lowering) |
| `07_modules.br` (+ `07_lib.br`) | modules, `#include` composition, shared facts | `... print src/showcase/07_modules.br` |

Other views and tools:

- `... print --c-like <file>` — mini-C++ rendering (ids kept as `// nNN` comments)
- `... to-json <file>` — the same term model as JSON
- `... schema [--core-only]` — the loaded vocabulary (scalars, kinds, traits)
- `... facts <file>` — the fact store with scopes
- `... check --core-only <file>` — see what the core module alone provides
- `... fold-reshape src/examples/reshape_facts.br` — a pass deciding from facts

Design notes behind each feature live in `docs/scratch/` (`scoping.md`,
`control-flow.md`, `ssa-joins.md`, `symbolic-inference.md`, `connectors.md`,
`schema-registry.md`, `paradigm.md`).
