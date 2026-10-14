# Brrr

**B**ot-**R**eadable, **R**easoned, **R**eproducible. Compiler go brrr.

Brrr is a framework for an agent to optimize **one kernel**, or one piece of
code, until it compiles to a stupidly fast binary. It is not trying to represent
every IR or to be right for every possible input of every program. The agent
says which inputs this kernel actually gets, specializes hard for exactly those,
and every step along the way is checked.

## What it's for

The loop an agent runs: read the kernel, state what it knows, transform it, keep
what is provably right, throw away what isn't, and try again. Everything in the
core serves that loop:

- **Read exactly what the kernel computes.** Every node has a stable id that
  survives every pass and every edit. Shapes and integer operands are
  expressions over SSA values, so `tensor<[2 * %n, 64], f16>` says what it
  means.
- **Specialize to the inputs that matter.** Facts state what the agent knows or
  chooses to assume, for example `fact %n mod 128 == 0` or `fact 1 <= %batch <= 8`.
  Everything after that is proven under those facts, nothing more. Whatever
  can't be proven becomes a runtime guard, so a binary specialized for the
  inputs you care about fails loudly on the others instead of computing
  garbage.
- **Check every claim.** A solver (Z3, with local rules as a fallback and
  cross-check) answers every shape equation with Yes, No or Unknown, and never
  guesses.
- **Keep every attempt.** Compilation is a recorded history. Every stage stores
  the IR before and after it, and any stage can be inspected, branched from, or
  resumed with a different pipeline or a hand-edited IR.

What stays out of the core is anything a single kernel doesn't need from every
user. Dependence analysis, polyhedral scheduling and parametric type systems
can each be a pass or a schema file when a kernel needs them. For example, a
kernel can bring its own types and ops
([custom type example](src/examples/custom_types.br)). The core keeps the
parser, the IR, the vocabulary as data, the solver, and the run store. A pass
is one C++ file.

## What it looks like

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

Compared with MLIR: nodes keep their identity (MLIR's printer renumbers
`%0, %1, ...` on every print), dims are expressions instead of `?` plus
`OpFoldResult` plus affine maps, and a compile is a history you can branch
rather than a one-shot pipeline.

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

## Running a pipeline

A pipeline is a text file, one pass per line. `brrr passes` lists the passes:

```
infer-shapes
canonicalize
insert-guards
dce
```

Every run gets an id and is recorded under `.brrr/`:

```
brrr run kernel.br --pipeline=opt.txt        # run r1: verify the input, then stage by stage
brrr show r1@2                               # the IR after stage 2
brrr show r1@2 --record                      # what stage 2 did: diff, journal, issues
brrr branch r1@1 --passes=canonicalize,dce   # from stage 1, try another way (run r2)
brrr resume r1 --state=edited.br             # hand-edit the IR (ids are recovered), continue
brrr runs                                    # every run, its status, and its parent
```

The rules an agent can rely on:

- **Every stage is verified.** If a pass leaves errors behind, or produces IR
  that doesn't round-trip through text, the stage is rolled back. The run
  stops at the last good state and prints what to try next.
- **Stop to look.** `--steps=N` runs N passes and pauses; `--pause-on-warning`
  pauses as soon as a stage leaves a warning, such as an equation the solver
  couldn't prove. `resume` continues, and `.brrr/runs/<id>/pipeline.txt` holds the
  passes still to run, so you can edit it first.
- **Branch instead of undoing.** A branch starts from any stage and by default
  replays the rest of the parent's pipeline. Give it `--passes` or `--pipeline`
  to try something else. The parent run is never modified.
- **Edits are stages too.** `--state=<file>` turns a hand-edited IR into a
  recorded stage. Node ids come back by name matching, so the history still
  lines up.

The [pipeline walkthrough](src/showcase/README.md#compiling-runs-stages-branches)
shows a full session.

## Status

Prototype. What works and is tested (four `ctest` suites, ~170 CLI checks):

- the v1 language ([spec](docs/scratch/syntax-v1.md)): parser, both printings
  (clean, and stored with ids), round-trip, recovering ids from edited clean text;
- vocabulary as schema data: operands, successors, typed attributes, result
  types, symbolic meanings (`expr:`), shape rules (`shape:`); custom type names
  need no core support;
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

Not yet: code generation, so there is no binary yet, fast or otherwise (the plan
goes through MLIR's text format to LLVM). Also missing: a rewrite-pattern
engine, call ops and preconditions at call sites, `min`/`max` in expressions,
and any measurement. The core builds in seconds, but neither compile time nor
generated code has been benchmarked. The roadmap is
[hockyy/brrr#1](https://github.com/hockyy/brrr/issues/1).

## Layout

| Path | Role |
|---|---|
| `src/` | the library and the `brrr` CLI ([README](src/README.md)) |
| `src/showcase/` | a guided tour, one concept per file |
| `src/examples/` | external IRs mapped in (LLVM, torch.fx, AscendNPU HIVM, `scf.while`) and a kernel with its own types |
| `docs/scratch/` | design notes and the decision log; [`syntax-v1.md`](docs/scratch/syntax-v1.md) is the spec |
| `docs/` | the paper draft (LaTeX; build with `latexmk -pdf main.tex`) |
