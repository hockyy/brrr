# Control flow in BridgeIR (v0): branches and loops, completely

> **2026-10-13 supersede:** `phi` has been **removed** from the language. Wherever
> this note says phi is declared or retained, read it as superseded: a block's
> arguments, fed by its incoming `edge` values, are the only way a value crosses a
> block boundary. `src/examples/dot_llvm.br` (clang output) is mapped without phi.
> Also: scope headers now carry typed arguments by id (`block (int64 n9)`),
> `param` lines are normalized away, and argument names are gone. See `design-notes.md`.

Status: implemented 2026-10-13. Mechanism: **any node opened with `{` is a scope.**
Modules, funcs, CFG blocks, and (future) loop bodies are uniformly scope nodes with
stable ids, so control flow needs no new core concepts — only declared kinds.

## Branches: blocks as scopes

```
func @sum_to_n(n1) {
  n5 entry = block {
    n6 has = icmp n_val zero pred="gt"
    n7 = cbr has body exit          # targets are block VALUES (names or ids)
  }
  ...
}
```

- `block` produces a value (`results: value` in the schema): control is data, and
  branch ops consume it. No special "block reference" machinery.
- `br { operands: target }`, `cbr { operands: cond, if_true, if_false }` — declared
  and validated like every other kind.
- Blocks are **scopes**: instructions nest inside; membership is structural
  (`childrenOf(block)`), not a strawman `in=` attribute.
- `phi { operands: value* }` lists `(value, from-block)` pairs at block entry.

## Scoping rule (important, and deliberate)

Names are **scope-local**: a name resolves through the scope chain
(block → func → module → root). A value defined inside one block is *not* visible
by name from a sibling block — **cross-block references use stable ids**
(`n25` in the dot example). That asymmetry is the point:

- names are for humans/agents reading a local region;
- ids are for the machine and survive any restructuring.

The printer preserves both: it emits a name when it resolves back to the same node,
an id otherwise; round-trips are stable either way.

## Loops

A CFG loop is just blocks + phis + a conditional branch — no new concepts:

```
n8 body = block {
  n9  acc = phi zero entry new_acc body    # loop-carried accumulator
  n10 i   = phi n_val entry next_i body    # loop counter
  n11 next_i = arith.addi i neg_one
  n12 new_acc = arith.addi acc i
  n13 more = icmp next_i zero pred="gt"
  n14 = cbr more body exit
}
```

Complete, lowering-faithful, and already round-trip-tested:
`examples/loop_cfg.br` (canonical form) and `examples/dot_llvm.br` (a real
`clang -O1` loop mapped nearly one-to-one).

## Simple loop form (structured — implemented)

For authored IR, the CFG form is more ceremony than needed. The same mechanism
(scope + block arguments) yields a compact loop kind:

```
n5 sum = loop zero n_val {        # operands: initial carried values
  n6 acc = param idx=0            # carried values are the scope's parameters
  n7 i = param idx=1
  fact 0 <= i                     # invariants still scope to the body
  ...
  n11 = loop.yield more new_acc next_i   # (continue-condition, next values)
}
n12 = func.return sum.0           # exit values are the loop's ports
```

Rules (relational checks): parameter count == initial operands; exactly one
`loop.yield` in the body; yield operands == 1 + parameter count (condition first).
`loop.N` ports are the exit values, in parameter order. Compared to the CFG form:
one scope instead of blocks/edges/cbr, carried values are just parameters, and the
exit is a port rather than another edge. The CFG form remains for lowering
fidelity and imports (like phi); the structured form is what a human or agent
should write first. See `examples/loop_simple.br` vs `examples/loop_cfg.br`.

## C++-shaped sugar: `for`, `while`, `if/else` (implemented)

Surface sugar, lowered textually *before* parsing; the canonical form stays
dataflow, so nothing in the core, schema, or checks changes:

```
n5 sum  = for (acc = zero, i = n_val; more; acc = new_acc, i = next_i) { ... }
n14 w   = while (more) { ... i = next ... }        # carried names from assignments
n5 pick = if (greater) { n6 = yield a } else { n7 = yield b }
```

Lowering: `for`/`while` → `loop` + `param`s + `loop.yield`; `if` → `cbr` + branch
`block`s + `edge`s + a join `block` that carries the node id/name (so `pick` is the
join's value and `pick.1` its second port).

Honest boundaries:

- Conditions, step values and assignment right-hand sides are **SSA value names or
  ids**, not expressions. `i = i - 1` needs expression/assignment lowering — a
  frontend feature (SSA construction), the documented next level.
- `while` discovers its carried names from depth-0 body assignments; the initial
  value is the enclosing binding of that name (C++ semantics: state before loop).
- Value-`if` only: both branches, exactly one `yield` each, equal arity.
- Everything else still applies: facts scope to their block (`ask n10 "a >= b"` in
  a branch answers), the verifier checks edges/params, and printing shows the
  canonical lowered form (`print` on `loop_cpp.br` / `branch_cpp.br`).

## Block arguments vs phi nodes (both declared; block args are canonical)

| | phi nodes (LLVM) | block arguments (MLIR, SIL, BridgeIR) |
|---|---|---|
| placement | grouped at block top; every pass special-cases them | parameters of the block — uniform iteration |
| semantics | parallel copy at *block entry*, entangled with ordering (the "lost copy"/swap bugs of SSA destruction) | parallel copy at the *edge*: the branch site states what it passes |
| function inputs | separate Argument concept | unified: function inputs and block inputs are both `param` nodes |
| cross-block refs | needed (defs live in other blocks) | none: parameters are block-local names |

BridgeIR form: an `edge` node carries `(target, passed...)`; `br`/`cbr` consume
edge values; blocks declare `param idx=N` arguments. Arity is checked by the
schema via traits (`block`/`edge`/`arg`): edge passed-values must equal the target
block's parameter count — violations are rejected (`tests/cases/bad_edge.br`).

```
n5 entry = block {
  n6 has = icmp n_val zero pred="gt"
  n7 e_body = edge body zero n_val        # what is "passed in"
  n8 e_exit = edge exit zero
  n9 = cbr has e_body e_exit
}
n10 body = block {
  n11 acc = param idx=0                    # block-local; no cross-block ids
  n12 i = param idx=1
  ...
  n16 e_again = edge body new_acc next_i   # what is "looped back"
  ...
}
```

`phi` was removed (2026-10-13): clang output maps onto block arguments fed by
edges, so no phi→args normalization pass is needed — the mapping is direct.

## Structured regions (future, reserved now)

The same scope mechanism will carry MLIR-style structured control without new
syntax:

```
n20 loop = for zero n_val {        # scope node with a body
  ...                              # region arguments later: loop-carried values
  n25 = yield next_acc             # `yield` is already declared and reserved
}
```

What it needs before it lands: **region arguments** (reading the carried value
inside the body without id tricks) and multi-port results (`loop.0`, `loop.1` —
the port field exists; arity validation does not yet). Beyond that, the mechanism
is the one shipped above.

## Why both representations, eventually

| | CFG (v0, implemented) | Structured (future) |
|---|---|---|
| Fidelity to LLVM/machine lowering | exact | needs structure-preserving passes |
| Fidelity to graph faces (FX, torch) | noisy | natural |
| Loop-carried values | `phi` pairs | `yield` + region args |
| Verifier cost | dominance checks | structural checks |

Start with CFG because every backend already speaks it; add structured regions as
a *dialect layer* on the same scope mechanism when graph frontends need them.

## What changed for it

- Node field `has_body` (parser records `{`, printer/JSON/C-like view respect it).
- One printer path for `func`/`module`/`block`/anything braced.
- Declared: `block`, `br`, `cbr`, `phi`, `yield` (+ `icmp`, `arith.constant`,
  `arith.addi` as common scalar vocabulary).
