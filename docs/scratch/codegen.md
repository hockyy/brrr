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

## Not yet

- **Tensor-level ops** (`matmul`, ATen ops): lower them to loops over buffers first (#19).
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
