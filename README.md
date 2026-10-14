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
Compiling to a binary needs clang, and the PyTorch frontend needs torch.

Then follow the tour in [`src/showcase/`](src/showcase/README.md): identity,
facts, vocabulary, control flow, loops, symbolic shapes, modules, pipeline runs,
the affine bridge, and a solver-backed pass.

## How an agent drives Brrr

Here is one real session, on the PyTorch kernel in [`examples/`](examples/): a
transformer feed-forward block, `y = x + fc2(relu(fc1(x)))`, over a token count
that is a multiple of 8. Every command below was run, and the outputs are
shortened only where marked. Everything an agent reads is plain text: IR,
answers, stage records. Nothing needs a library binding.

**1. Get the kernel.** `torch.export` writes it as Brrr. Shapes stay
expressions, torch's ranges become facts, and the ATen ops are declared as
schema data with shape rules:

```
$ python examples/kernel.py
$ brrr check examples/kernel.br --schemas=examples/aten.bs
errors: 0, warnings: 0                  # Brrr re-proved every shape torch inferred
```

```
func @forward(%t: index, %fc1_weight: tensor<[256, 64], f32>, ..., %x: tensor<[8 * %t, 64], f32>)
    -> tensor<[8 * %t, 64], f32> {
  fact 2 <= %t <= 512
  %linear = aten.linear %x, %fc1_weight, %fc1_bias : tensor<[8 * %t, 256], f32>
  ...
```

**2. Ask before acting.** The solver answers Yes, No or Unknown, and never
guesses. An Unknown means the answer depends on the input, which tells the agent
what it could still decide:

```
$ brrr ask examples/kernel.br @forward "8 * %t <= 4096" --schemas=examples/aten.bs
8 * %t <= 4096 -> Yes (scope @forward)
$ brrr ask examples/kernel.br @forward "(8 * %t) mod 8 == 0" --schemas=examples/aten.bs
(8 * %t) mod 8 == 0 -> Yes (scope @forward)
$ brrr ask examples/kernel.br @forward "%t <= 64" --schemas=examples/aten.bs
%t <= 64 -> Unknown (scope @forward)
```

**3. Run a pipeline. Every stage is recorded and verified.** `lower-aten` turns
the ATen ops into loop nests over buffers, and every load and store is proven in
bounds:

```
$ brrr run examples/kernel.br --passes=lower-aten --schemas=examples/aten.bs
run r1
  [0] input examples/kernel.br  state 731a5919
  [1] lower-aten  ok  0.391 ms  +121 -5 ~6  state 358e62ee
$ brrr show r1@1 --record                # what the stage did, by node id
what lower-aten
status ok
diff +121 -5 ~6
journal lower-aten n13: result -> output buffer argument
journal lower-aten n1: 4 ATen op(s) lowered to loops
...
```

A stage that breaks the IR is rolled back, and the run stops at the last good
state with a list of what to try next.

**4. Specialize: narrow the inputs.** Say this binary only ever serves 512
tokens. The agent edits the IR as text: it adds one line, a guard (a fact that is
checked at run time), and resumes the run. Node ids are recovered from the edited
text, so the edit is just another recorded stage:

```
$ brrr show r1 > k.br                    # add after the facts:
                                         #   guard %t, 64 rel=eq what="this binary serves 512 tokens"
$ brrr resume r1 --state=k.br --passes=canonicalize,dce
  [2] edit k.br  ok  +1 -0 ~0
  [3] canonicalize  ok  58.346 ms  +0 -0 ~11
  [4] dce  ok  0.213 ms  +0 -0 ~0
$ brrr show r1 | grep "icmp 0,"
  %linear_i_enter = icmp 0, 512 pred=lt   # 8 * %t is now 512 everywhere below the guard
  ...
```

Called with any other token count, the specialized binary traps instead of
computing garbage.

**5. Try another way without losing this one.** A branch starts from any stage of
any run, and the parent run is never modified:

```
$ brrr branch r1@1 --passes=canonicalize   # run r2: the generic kernel, another pipeline
$ brrr runs                                # every run, its status, and its parent
```

**6. Compile and measure.** `brrr to-llvm` writes LLVM IR, and clang does the
rest. `examples/build.py` runs the whole path and checks the result against
torch's own output:

```
$ brrr show r1 > special.br && brrr to-llvm special.br > special.ll
$ clang -O3 -march=native special.ll examples/harness.c -o special
$ python examples/build.py               # kernel.br -> lower-aten -> to-llvm -> clang -> compare with torch
$ python examples/build.py --passes "lower-aten,allow-reassoc"   # the same, with one niching step
$ python examples/build.py --passes "tile-linear rows=4 cols=2,lower-aten,allow-reassoc contract=true"
```

## From PyTorch to a binary: where it stands

`python examples/build.py --passes "<pipeline>"` on this machine (Arrow Lake,
AVX2 + FMA). Every row matches torch's output within 6e-7:

| tokens | `lower-aten` (naive loops) | `+allow-reassoc` | `tile-linear rows=4 cols=2,` ... `contract=true` | torch (all threads) | torch (1 thread) |
|---|---|---|---|---|---|
| 32 | 0.28 ms | 0.052 ms | **0.038 ms** | 0.078 ms | 0.028 ms |
| 512 | 4.2 ms | 0.85 ms | **0.52 ms** | 0.090 ms | 0.27 ms |
| 4096 | 34.2 ms | 6.5 ms | **3.85 ms** | 0.30 ms | 2.3 ms |

**The first niching step was one recorded permission: `allow-reassoc`.** Float
addition isn't associative, because `(a + b) + c` and `a + (b + c)` can round
differently. So without permission LLVM must sum a dot product strictly in
order. That is a serial chain of adds, each waiting about 4 cycles for the one
before it, and it can't use the CPU's 8-wide vector lanes. `allow-reassoc` finds
the loop reductions (here, the two linear layers' k-loops) and marks just those
adds `reassoc=true`. LLVM then keeps vector partial sums and combines them at
the end. That made the kernel **about 5.5× faster**. The results still match
torch, whose own matmul doesn't sum in order either. The permission is an op
attribute, so it is visible in the IR, and each grant is logged in the stage
record by node id.

**The second was a schedule: `tile-linear`** ([#25](https://github.com/hockyy/brrr/issues/25)).
The loop was now limited by loads, since every multiply-add read one `x` and one
`w`. `tile-linear rows=R cols=C` asks the solver whether the tile divides the
layer (`8 * %t mod 4 == 0`: Yes, for every `%t`) and records the tile on the op.
`lower-aten` then computes an R x C block of outputs per k loop, so each loaded
value feeds several multiply-adds. With the loads amortized, `contract=true`
(FMA) finally pays. A tile that need not divide is refused, and the refusal
names the question (`rows=6 needs 8 * %t mod 6 == 0`), so the agent knows which
fact or divisor to try. `form=outer` transposes the weights once and vectorizes
across columns instead; every output is then summed in order (no `reassoc`), and
it is close behind (3.9 ms at 4096 tokens).

**What's left is memory, not arithmetic.** At 4096 tokens the kernel allocates
9 MB of intermediates on every call, and the OS hands back fresh pages each
time. With the very same binary and an allocator that reuses buffers between
calls, it runs 0.026 / 0.27 / 2.30 ms: level with torch on one thread. The
inner loop is already a textbook 4x16 FMA micro-kernel. Next, each step a run an
agent can branch and measure:

- **No per-call heap traffic** ([#28](https://github.com/hockyy/brrr/issues/28)):
  fuse relu and the residual add into the linear epilogues, run the two layers
  row block by row block so the 256-wide hidden activations never leave L1, and
  give the caller a workspace instead of `malloc`.
- **Facts as LLVM hints** ([#26](https://github.com/hockyy/brrr/issues/26)): what
  Brrr proved becomes `noalias`, `llvm.assume` and `align`.
- **Threads**, to meet torch's all-thread numbers at 512+ tokens.

## Running a pipeline: the reference

A pipeline is a text file, one pass per line; `brrr passes` lists the passes:

```
infer-shapes
canonicalize
insert-guards
dce
```

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
  couldn't prove. `resume` continues, and `.brrr/runs/<id>/pipeline.txt` holds
  the passes still to run, so you can edit it first.
- **Branch instead of undoing.** A branch starts from any stage and by default
  replays the rest of the parent's pipeline. Give it `--passes` or `--pipeline`
  to try something else. The parent run is never modified.
- **Edits are stages too.** `--state=<file>` turns a hand-edited IR into a
  recorded stage. Node ids come back by name matching, so the history still
  lines up.
- **Unknown is never Yes.** An equation the solver can't prove is a warning
  (an error under `check --strict`), and `insert-guards` turns it into a runtime
  check. An array index that can't be proven in bounds gets the same treatment.

## Status

Prototype. What works and is tested (four `ctest` suites, ~200 CLI checks):

- the v1 language ([spec](docs/scratch/syntax-v1.md)): parser, both printings
  (clean, and stored with ids), round-trip, recovering ids from edited clean text;
- vocabulary as schema data: operands, successors, typed attributes, result
  types, symbolic meanings (`expr:`), shape rules (`shape:`); custom type names
  need no core support;
- verification: structure, dominance, successor and loop arity; shape rules
  checked by the solver; facts checked by role (invariants on every incoming
  edge, with branch conditions; loops by induction; lemmas), and facts flowing
  into dominated blocks;
- buffers, with every `load`/`store` proven in bounds or guarded at run time;
- expressions with `floordiv`/`ceildiv`/`mod` by constants, and an exact bridge
  to MLIR affine maps both ways;
- the solver: Z3 when available, otherwise local rules; `BRRR_SOLVER=check`
  asks both, and the test suite fails if they ever disagree
  ([solver notes](docs/scratch/solver.md));
- PyTorch in (`examples/kernel.py`, `torch.export`), native code out
  (`brrr to-llvm` + clang), checked against torch ([codegen notes](docs/scratch/codegen.md));
- passes, one self-registering C++ file each: `infer-shapes`, `fold-constants`,
  `fold-reshape`, `insert-guards`, `reconcile-casts`, `dce`, `canonicalize`,
  `lower-aten`, `allow-reassoc`, `tile-linear`;
- the run store: `run`, `resume`, `branch`, `show`, `runs`, with hand edits as
  recorded stages and rollback of any stage that fails verification.

Not yet: most niching (so far `allow-reassoc` and `tile-linear`, see above), a
rewrite-pattern engine, call ops and preconditions at call sites, `min`/`max`
in expressions, and vector types. The roadmap is
[hockyy/brrr#1](https://github.com/hockyy/brrr/issues/1).

## Layout

| Path | Role |
|---|---|
| `examples/` | PyTorch in and a binary out: `kernel.py` (torch.export → `kernel.br`), `build.py` (→ binary, checked against torch), `harness.c` |
| `src/` | the library and the `brrr` CLI ([README](src/README.md)) |
| `src/showcase/` | a guided tour, one concept per file |
| `src/examples/` | external IRs mapped in (LLVM, torch.fx, AscendNPU HIVM, `scf.while`) and a kernel with its own types |
| `docs/scratch/` | design notes and the decision log; [`syntax-v1.md`](docs/scratch/syntax-v1.md) is the spec |
| `docs/` | the paper draft (LaTeX; build with `latexmk -pdf main.tex`) |
