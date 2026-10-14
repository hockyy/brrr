# Brrr showcase — a tour of the language

One concept per file. Every file verifies and round-trips in the test suite, and
every "Try:" in a file's comments is checked too. Run from the repo root after
building (`cmake --build src/build`); `brrr` below means `src/build/brrr`.

| File | Shows | Try |
|---|---|---|
| `01_identity.br` | stable ids, clean vs stored form, rename | `brrr print 01_identity.br --ids` · `brrr rename 01_identity.br %sum %total` |
| `02_facts.br` | facts and their roles: assumption, precondition, lemma | `brrr ask 02_facts.br @split "%n == 128 * %tiles"` → Yes |
| `03_vocabulary.br` | kinds and attributes as schema data; foreign kinds | `brrr schema` · change `pred=gt` to `pred=gtt` |
| `04_cfg.br` | blocks with arguments; invariants proven on every edge | `brrr ask 04_cfg.br ^body "%i2 + 1 == %i"` → Yes |
| `05_loop.br` | a structured loop; invariants by induction | delete the precondition, then `check` |
| `06_shapes.br` | symbolic shapes, shape rules, fresh dims `?` / `dim(%x, k)` | `brrr types 06_shapes.br` |
| `07_modules.br` + `07_lib.br` | modules and `#include` | `brrr print 07_modules.br` |
| `08_pipeline.br` + `.txt` | a recorded, branchable compile | see below |
| `09_affine.br` | `floordiv`/`ceildiv`/`mod`, `apply`, the MLIR affine bridge | `brrr to-affine 09_affine.br @pinned "%mult"` → static 4 |
| `10_canonicalize.br` | a solver-backed pass in one C++ file: Z3 proposes, the solver proves | `brrr run 10_canonicalize.br --passes=canonicalize` · `brrr show r1@1 --record` |

## Compiling: runs, stages, branches

```
$ brrr run src/showcase/08_pipeline.br --pipeline=src/showcase/08_pipeline.txt
run r1
  [0] input src/showcase/08_pipeline.br  state 98d80e92
  [1] infer-shapes  ok  0.020 ms  +0 -0 ~1  state eb3c7b4f
  [2] fold-reshape  ok  0.024 ms  +0 -2 ~1  state 9067baaa
  [3] dce  ok  0.001 ms  +0 -0 ~0  state 9067baaa
status: done — final state r1@3

$ brrr show r1                 # the final state
func @roundtrip(%n: index, %m: index, %x: tensor<[%n, %m], f32>) -> tensor<[%n, %m], f32> {
  %y = relu %x : tensor<[%n, %m], f32>
  return %y
}

$ brrr show r1@2 --record      # what stage 2 did, and why
journal fold n6: reshape of reshape n5 composed
journal fold n6: reshape to its own type is the identity
journal replace n7: %6 -> %4
...
```

What you control:

- **Inspect any stage**: `show r1@k` (clean), `--ids` (stored form), `--record`.
- **Step**: `run ... --steps=1` pauses after one stage; `--pause-on-warning` pauses
  when a stage leaves an unproven equation.
- **Change the plan**: edit `.brrr/runs/r1/pipeline.txt` (the passes still to
  run), then `resume r1`; or `resume r1 --passes=a,b`.
- **Change the IR by hand**: `show r1 > x.br`, edit it, `resume r1 --state=x.br`.
  The edit becomes a recorded stage; ids are recovered by matching names, so
  everything you did not touch keeps its identity.
- **Try another way**: `branch r1@1 --passes=dce` starts run r2 from r1's stage 1;
  r1 is untouched. `runs` lists every run and where it came from.
- **Nothing bad sticks**: after every stage the state is verified and must
  round-trip through text. A stage that fails is rolled back, the run stops at
  the last good state, and the rejected state is kept for inspection.

Design notes behind each feature live in `docs/scratch/`; the language spec is
`docs/scratch/syntax-v1.md`.
