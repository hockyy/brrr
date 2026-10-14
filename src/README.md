# src

The Brrr library and CLI. C++20, no MLIR or LLVM dependency in the core.
The language is specified in [`../docs/scratch/syntax-v1.md`](../docs/scratch/syntax-v1.md).

## Build & test

    cmake -S src -B src/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build src/build
    ctest --test-dir src/build --output-on-failure

Z3 is optional: CMake looks in `third_party/z3` (override with
`-DBRRR_Z3_ROOT=<dir>`) and the system paths, and without it the local rules
decide alone. See [`../docs/scratch/solver.md`](../docs/scratch/solver.md);
`BRRR_SOLVER=z3|local|check` picks the engine at run time.

Suites: `core` (CLI checks in `tests/run_tests.sh`, with the negative cases in
`tests/cases/` declaring the error they expect), `symbolic` (solver),
`types` (unification), `connector` (SymPy escalation; needs Python with sympy).

## Layout

| Directory | What |
|---|---|
| `ir/` | the IR model (`Node`, `Module`, journaled edits) and the text form: lexer, parser, both printers, id reconciliation |
| `symbolic/` | expressions, facts, and the solver: local rules (polynomial normal form, linear inequalities, intervals, residues) and the Z3 backend (`z3_backend.cpp`) |
| `schema/` | the vocabulary as data, and the standard `core` / `std` / `control` modules |
| `verify/` | structural checks: arity, attributes, terminators, dominance, loops |
| `sema/` | meanings, types and shape rules, fact roles, `verifyModule` |
| `passes/` | one file per pass, each self-registering (`pass.h` explains how to write one) |
| `pipeline/` | the run store: runs, stages, branches, edits |
| `io/` | JSON encoding |
| `interop/` | the MLIR affine bridge: affine maps <-> expressions |
| `codegen/` | `to-llvm`: LLVM IR text for scalars, buffers and blocks ([notes](../docs/scratch/codegen.md)) |
| `connectors/` | external solvers (SymPy), consulted only when the local solver says Unknown |
| `types/` | the unification prototype (`ctest -R types`) |

## Libraries

| Target | Contents |
|---|---|
| `brrr_core` | the API: `ir/`, `schema/`, `symbolic/`, `verify/`, `sema/`, `interop/`, `io/`, `connectors/` |
| `brrr_passes` | every `passes/*.cpp`, globbed (adding a pass edits no build file) |
| `brrr` | the CLI: `tools/brrr_main.cpp` and the run store in `pipeline/` |

## Writing a pass

A pass is one `.cpp` file in `passes/` that ends with
`BRRR_PASS(id, "name", "summary", fn)`. Reconfigure, and it shows up in
`brrr passes` and works in pipelines. It edits the module only through the
journaled `Module` methods. After each stage the run store verifies the module
and round-trips it through text, so a broken pass is rolled back rather than
trusted. `canonicalize.cpp`, at about 80 lines, is a complete example.

## Commands

    brrr print  <file> [--ids]          # clean form, or stored form with ids
    brrr check  <file> [--strict]       # --strict: unproven equations are errors
    brrr types  <file>                  # each value's type and meaning
    brrr ask    <file> <scope> "<q>"    # scope: root | @func | ^block | nID
    brrr facts  <file>
    brrr rename <file> %old %new
    brrr to-json <file>  |  roundtrip <file>  |  schema
    brrr to-llvm <file>                     # LLVM IR text; then clang -O3 x.ll ...
    brrr to-affine <file> <scope> "<e1>, <e2>"     # MLIR export, per OpFoldResult
    brrr from-affine "<affine_map>" <dims...> <symbols...>   # MLIR import

    brrr passes
    brrr run    <file> --pipeline=<file> | --passes=a,b  [--steps=N] [--pause-on-warning]
    brrr resume <run> [--pipeline=..|--passes=..] [--state=<edited.br>] [--steps=N]
    brrr branch <run>@<k> [--pipeline=..|--passes=..] [--state=<edited.br>]
    brrr runs
    brrr show   <run>[@k] [--ids] [--record]

Runs live in `.brrr/` (or `--store=<dir>`): content-addressed states, one
manifest per run, an editable `pipeline.txt` of passes still to run, and a record
per stage (before/after, diff, journal, issues), and `answers.txt`, the recorded
solver answers that make branches and resumed runs replay exactly.

## Notes

- Windows (MinGW): the runtime is linked statically so a different
  `libstdc++-6.dll` on PATH cannot be loaded by mistake; output is LF-only.
- With Z3 on Windows, `libz3.dll` and its MSVC runtime DLLs are copied next to
  the binaries at configure time.
- The C-like view and the C++-shaped sugar of v0 are gone; see the "Removed"
  table in the spec.
