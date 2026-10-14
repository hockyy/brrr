# Codegen: LLVM IR text

`brrr to-llvm <file>` writes LLVM IR as text, and clang turns it into a binary.
Brrr links no LLVM library. The module must verify first; anything without a
lowering is an error that names the node.

```
brrr to-llvm src/tests/codegen/dot.br > dot.ll
clang -O3 -march=native dot.ll harness.c -o dot
```

## Mapping

| Brrr | LLVM |
|---|---|
| `index`, `iN`/`uN` | `i64`, `iN`; signedness picks `sdiv`/`udiv`, `slt`/`ult`, `sext`/`zext` |
| `f16`, `f32`, `f64` | `half`, `float`, `double` |
| `buf<[dims], e>` (any shaped type) | `ptr`; row-major and contiguous |
| `func @f(...) -> T` | `define T @f(...)`; the ops in the function's own scope form the entry block `.entry` |
| `^block(%a: T)` | a basic block; its arguments become `phi` nodes, one incoming value per branch edge |
| expression operands | `i64` arithmetic: `add`/`mul nsw` (index arithmetic never wraps, by the soundness rule); `floordiv`/`ceildiv`/`mod` by a constant become `sdiv`/`srem` plus a rounding correction |
| `const` | an immediate (floats in hex, so they are exact) |
| `add`/`sub`/`mul` | integer ops (`nsw` for `index` and with `nsw=true`, `nuw` with `nuw=true`), or `fadd`/`fsub`/`fmul` on floats |
| `relu` | `llvm.maxnum` on floats, `select` on integers |
| `icmp pred=lt` | `icmp slt` (`ult` for `uN`), `fcmp olt` on floats |
| `cast` | `sext`/`zext`/`trunc`/`sitofp`/`uitofp`/`fptosi`/`fptoui`/`fpext`/`fptrunc`, by the two types |
| `load %b, [i, j]` | `getelementptr inbounds` on the row-major offset `i * d1 + j`, then `load` |
| `alloc` / `dealloc` | `malloc` of `product(dims) * sizeof(e)` / `free` |
| `guard a, b rel=le` | `icmp sle` and a branch to a block that calls `llvm.trap` |
| `return` | `ret` |

## From PyTorch: `lower-aten`

The `lower-aten` pass turns the frontend's ATen ops into loop nests over buffers:

```
brrr run examples/kernel.br --passes=lower-aten --schemas=examples/aten.bs
```

- **Signature.** Tensor arguments become buffers. The result becomes an output
  buffer argument (destination-passing), and the returned value is computed
  straight into it. Intermediates are `alloc`ed and freed at the end.
- **`aten.linear`:** `for i < B, n < N: acc = b[n]; for k < K: acc += x[i,k] * w[n,k]; out[i,n] = acc`.
  The accumulator is a block argument.
- **`aten.relu`, `aten.add`, `aten.mul`:** elementwise loops over every dimension.
- **Every loop** comes from `Builder::countedLoop`: a zero-trip check, then a header
  whose fact `0 <= iv <= bound - 1` is proven on both edges into it. With facts
  flowing to dominated blocks, every generated load and store is proven in bounds,
  and the lowered kernel verifies with no warnings.
- **An op without a lowering** fails the pass and names the op. The run store rolls
  the stage back.

## End to end: `examples/build.py`

```
python examples/build.py            # [--t 4,64,512] [--reps 20] [--work DIR]
```

One command runs the whole path:

1. `kernel.br` (written by `torch.export`)
2. `lower-aten`
3. `to-llvm`
4. `clang -O3 -march=native` with `examples/harness.c`
5. a run against torch's own output, timed alongside torch

The baseline, measured on this machine (Arrow Lake, AVX2 + FMA):

| tokens | max abs err | Brrr, naive loops | torch (all threads) | torch (1 thread) |
|---|---|---|---|---|
| 32 | 4.8e-7 | 0.27 ms | 0.075 ms | 0.029 ms |
| 512 | 4.8e-7 | 4.3 ms | 0.12 ms | 0.27 ms |
| 4096 | 6.0e-7 | 34.5 ms | 0.41 ms | 2.4 ms |

The naive loops are about 15× slower than single-threaded torch at 4096 tokens:

- **No vectorization.** The float reduction runs scalar, because LLVM may not
  reassociate it without permission.
- **No tiling.** Nothing keeps the weights in cache.
- **No facts used.** The fact `t >= 2` and the static inner dims are not exploited.

This is the starting point the agent's niching improves on, with each step
recorded as a run.

## Niching 1: `allow-reassoc`

Float `add`/`sub`/`mul` accept `reassoc=true` and `contract=true`. `to-llvm` emits
them as LLVM fast-math flags (`fadd reassoc contract`). On anything that isn't a
float they are refused, so they can never be a silent no-op.

The pass `allow-reassoc` grants `reassoc` to loop reductions. A reduction is a
float add that reads a block argument and whose sum a branch passes back into
that argument. With `contract=true`, the add and the multiply feeding it also get
`contract`; `reassoc=false contract=true` grants FMA alone. Each grant is
journaled, so the stage record says which ops may round differently and why.

| pipeline, 4096 tokens | ms | |
|---|---|---|
| `lower-aten` | 34.5 | baseline |
| `lower-aten,allow-reassoc` | 6.1–6.8 | about 5.5× faster: the vectorized reductions |
| `lower-aten,allow-reassoc contract=true` | 6.1–6.9 | no consistent change: the loop is now load-bound |

Every variant still matches torch within 6e-7. The next step is register-block
tiling (#25), which reuses loaded values.

## Niching 2: `tile-linear`

`tile-linear rows=R cols=C [form=inner|outer]` schedules every `aten.linear`. It
is a schedule, not a rewrite: it asks the solver `aligned(B, R)` and
`aligned(N, C)` under the facts in scope, and on two Yes answers it sets the
attributes `rows`, `cols`, `form` on the op and journals the proof. Anything but
Yes refuses the stage and names the question (`rows=6 needs 8 * %t mod 6 == 0,
and the solver says Unknown`). There are no remainder loops: the agent picks a
divisor, or adds the fact that makes one legal.

`lower-aten` realizes the tile. Per R x C block of outputs (loops `_ib`, `_nb`),
R * C accumulators start at the bias and are carried through one k loop, then
stored. Tile indices are expressions (`4 * %linear_ib + 3`), so the bounds proof
for every load and store goes through the solver like any other.

- `form=inner` (default): each accumulator is a dot product along k. It
  vectorizes only with `reassoc`, and each block ends with R * C horizontal sums.
- `form=outer`: the weights are transposed once per call into `w_t[k, n]` (loops
  `_tk`, `_tn`). For each k, a row's C outputs read C contiguous weights, so they
  vectorize across columns with a broadcast `x`. Every output is still summed in
  k order: exact rounding, no `reassoc`. With `contract` alone, clang emits the
  classic 4x16 AVX2 micro-kernel (two `w_t` loads, four broadcasts, eight FMAs).
  `reassoc` hurts this form (9.5 ms at 4096 tokens): it steers the vectorizer
  back to k.

| pipeline (ms at 32 / 512 / 4096 tokens) | 32 | 512 | 4096 |
|---|---|---|---|
| `lower-aten,allow-reassoc contract=true` | 0.052 | 0.85 | 6.5 |
| `tile-linear rows=4 cols=2,lower-aten,allow-reassoc` | 0.043 | 0.59 | 4.47 |
| `tile-linear rows=4 cols=2,lower-aten,allow-reassoc contract=true` | **0.038** | **0.52** | **3.85** |
| `tile-linear rows=4 cols=16 form=outer,lower-aten` | 0.088 | 0.57 | 4.03 |
| `... form=outer,lower-aten,allow-reassoc reassoc=false contract=true` | 0.087 | 0.58 | 3.93 |
| torch, one thread | 0.028 | 0.27 | 2.31 |

The sweep also covered 4x4, 4x8, 8x8, 8x16 and 2x16. The bigger tiles run out
of the 16 ymm registers, and 6x16 (the textbook AVX2 shape) is refused because 6
need not divide `8 * %t`. The outer form loses at 32 tokens because of the
per-call transpose (weights are arguments, so there is no "once").

**The gap to torch is allocation.** `lower-aten` allocates every intermediate
per call: 9 MB at 4096 tokens (the fc1 output, the relu output, the fc2 output).
On Windows, frees that size go back to the OS, so each call page-faults all of it
in again. Relinking the same `.ll` with a malloc that reuses buffers between
calls gives 0.026 / 0.27 / 2.30 ms, level with torch on one thread. That is the
next niche (#28).

## Niching 4: memory (`fuse-epilogue`, `fuse-rows`)

Two schedules on `aten.linear`. Like `tile-linear`, each records a decision as an
attribute, and `lower-aten` realizes it.

**`fuse-epilogue`** follows a linear's result through its readers. A reader
joins the chain when:

- it is the only reader of the value before it;
- it is `aten.relu`, or `aten.add` / `aten.mul` whose other operand already
  exists before the linear runs (an argument, or an earlier op).

The pass records `epilogue="relu"` (or `"relu,add"`, a chain) and journals what it
folded. A near miss is journaled too, for example `not fused: n7 (aten.add): its
other operand is computed after the linear`. `lower-aten` applies the chain to
each accumulator right before the store and stores into the last op's buffer. No
value inside the chain gets a buffer. In the FFN, fc2's chain ends at the
returned add, so fc2 writes `x + acc` straight into `%out`.

**`fuse-rows`**: when a linear's input is another linear's chain result, it is
the only reader, and both have the same `rows`, the consumer gets
`fuse_rows=true`. `lower-aten` then emits one row-block loop. Inside it, the
producer's column tiles write its R rows (after its epilogue) into an `R x N`
scratch, `%linear_rows = alloc : buf<[4, 256], f32>`, indexed by the tile row
alone (`[3, 2 * %linear_nb + 1]`). Then the consumer reads them from there.

`lower-aten` re-checks every schedule it realizes, the same way the pass decided
it (`passes/linear_chain.h`):

- an epilogue that no longer holds fails the stage: `epilogue step 'relu' no
  longer holds: its reader is add`;
- so does a `fuse_rows` with mismatched rows;
- so does a tile that isn't proven to divide its dim: `rows=3 is not proven to
  divide 8 * %t`.

A hand edit can't get a wrong schedule past it.

| pipeline (ms; one thread) | 32 | 512 | 4096 |
|---|---|---|---|
| `tile-linear rows=4 cols=2,lower-aten,allow-reassoc contract=true` | 0.038 | 0.52 | 3.85 |
| `tile-linear rows=4 cols=2,fuse-epilogue,lower-aten,allow-reassoc contract=true` | 0.017 | 0.26 | 2.61 |
| `tile-linear rows=4 cols=2,fuse-epilogue,fuse-rows,lower-aten,allow-reassoc contract=true` | **0.016** | **0.26** | **2.07** |
| `tile-linear rows=4 cols=16 form=outer,fuse-epilogue,fuse-rows,lower-aten,allow-reassoc reassoc=false contract=true` | 0.068 | 0.39 | 2.75 |
| torch, one thread | 0.028 | 0.27 | 2.28 |
| torch, all threads | 0.027 | 0.08 | 0.26 |

Run to run, the noise is about ±10%. `rows=2 cols=4` lands within it (1.96–2.36
ms at 4096); 1-row tiles and wider column tiles are slower. The kernel's only
allocation is the 4 KB scratch, plus `form=outer`'s 64 KB weight transposes,
which are served from the heap's free lists. So the workspace argument planned
in #28 isn't needed yet.

## Not yet

- **Other tensor-level ops** (`matmul`, and ATen ops beyond linear/relu/add/mul): lower them to loops first.
- **Structured `loop` regions:** write the loop as blocks.
- **Facts are not emitted.** Later they become `llvm.assume`, `!range`, `noalias` and `align`, so what Brrr proved turns into LLVM optimizations.
- **Vector types and parallel loops.**

## Tests

`src/tests/codegen/` holds the kernels and a C harness. When clang is present, the
suite compiles them and checks the results:

- a dot product (10100);
- a matrix-vector product (20 60 100);
- `sum_to_n(100)` (5050);
- an access guarded by `insert-guards`: it works in bounds and traps out of bounds.
