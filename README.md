# Brrr

**B**ot-**R**eadable, **R**easoned, **R**eproducible. Compiler go brrr.

A small compiler IR, written from scratch in C++20 with no MLIR or LLVM
dependency, built around three ideas MLIR does not have:

1. **Stable identity.** Every node keeps its id across every edit and every
   pass. MLIR has no operation identity beyond a pointer, and its printer
   renumbers `%0, %1, ...` on every print.
2. **Expressions as values.** A tensor dimension, a fact, or an integer operand is
   an expression over SSA values: `tensor<[2 * %n, 64], f16>`, `sub %i, 1`,
   `fact 0 <= %i <= %n`. A solver (Z3, with local rules as a fallback and
   cross-check) judges every shape equation as Yes, No or Unknown. Nothing is
   guessed, and an Unknown can become a runtime guard. This replaces MLIR's `?`
   dimensions, `OpFoldResult`, and affine maps.
3. **Compilation as a recorded, branchable history.** Every pipeline run has an id.
   Every stage stores the IR before and after it, and an agent (or a person) can
   inspect any stage, branch from it, edit the pipeline or the IR, and resume.

A `.br` file:

```
func @sum_to_n(%n: index) -> index {
  %zero = const 0 : index
  %has = icmp %n, %zero pred=gt
  cond_br %has, ^body(%zero, %n), ^exit(%zero)
^body(%acc: index, %i: index):
  fact 0 <= %i <= %n                # proven on both edges into ^body
  %i2 = sub %i, 1                   # to the solver, %i2 means %i - 1
  %acc2 = add %acc, %i
  %more = icmp %i2, %zero pred=gt
  cond_br %more, ^body(%acc2, %i2), ^exit(%acc2)
^exit(%r: index):
  return %r
}
```

## Try it

```
cmake -S src -B src/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build src/build
ctest --test-dir src/build --output-on-failure
```

Z3 is optional. CMake picks it up from `third_party/z3` or the system; without
it, the local rules decide alone ([solver notes](docs/scratch/solver.md)).

Then follow the tour in [`src/showcase/`](src/showcase/README.md): identity,
facts, vocabulary, control flow, loops, symbolic shapes, modules, pipeline runs,
the affine bridge, and a solver-backed pass.

PyTorch in: `python examples/kernel.py` exports a transformer feed-forward block
with `torch.export` and writes `examples/kernel.br`. torch's dynamic token count
`8 * Dim("t")` becomes the expression `8 * %t` in every type, its range becomes a
fact, and Brrr re-proves every shape torch inferred
(`brrr check examples/kernel.br --schemas=examples/aten.bs`).

And out to a binary: `python examples/build.py` lowers the kernel to loops
(`lower-aten`), emits LLVM IR (`brrr to-llvm`), compiles it with clang, and checks
it against torch's own output. It matches within 6e-7. These are naive loops, the
baseline before any specialization: about 15x slower than single-threaded torch
([codegen notes](docs/scratch/codegen.md)).

## Status

Prototype. What works and is tested (four `ctest` suites, ~170 CLI checks):

- the v1 language ([spec](docs/scratch/syntax-v1.md)): parser, both printings
  (clean, and stored with ids), round-trip, recovering ids from edited clean text;
- vocabulary as schema data: operands, successors, typed attributes, result
  types, symbolic meanings (`expr:`), shape rules (`shape:`);
- verification: structure, dominance, successor and loop arity; shape rules
  checked by the solver; facts checked by role (invariants on every incoming
  edge, with branch conditions; loops by induction; lemmas);
- expressions with `floordiv`/`ceildiv`/`mod` by constants, and an exact bridge
  to MLIR affine maps both ways (`to-affine` puts each operand on the right side
  of `OpFoldResult`: static attribute, value, or `affine.apply`);
- the solver: Z3 when available, otherwise local rules; `BRRR_SOLVER=check`
  asks both, and the test suite fails if they ever disagree
  ([solver notes](docs/scratch/solver.md));
- passes, one self-registering C++ file each: `infer-shapes`, `fold-constants`,
  `fold-reshape`, `insert-guards`, `reconcile-casts`, `dce`, and `canonicalize`
  (rewrites each integer expression to its simplest form that the solver proves
  equal);
- the run store: `run`, `resume`, `branch`, `show`, `runs`, with hand edits as
  recorded stages and rollback of any stage that fails verification.

Not yet: a rewrite-pattern engine, code generation (the plan is to emit LLVM IR
text), call ops and interprocedural preconditions, `min`/`max` in expressions, a
full MLIR importer/exporter (only affine maps so far), a binary format, and any
performance measurement against MLIR. "Faster than MLIR" is a goal, not a result:
the core builds in seconds, but compile-time speed has not been benchmarked.

## Layout

| Path | Role |
|---|---|
| `src/` | the library and the `brrr` CLI ([README](src/README.md)) |
| `src/showcase/` | a guided tour, one concept per file |
| `examples/` | PyTorch in: `kernel.py` exports a kernel with `torch.export` and writes `kernel.br` (symbolic shapes kept as expressions) |
| `src/examples/` | external IRs mapped in: LLVM, torch.fx, AscendNPU HIVM, `scf.while` |
| `docs/scratch/` | design notes and the decision log; [`syntax-v1.md`](docs/scratch/syntax-v1.md) is the spec |
| `docs/` | the paper draft (LaTeX; build with `latexmk -pdf main.tex`) |
