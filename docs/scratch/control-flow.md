# Control flow in BridgeIR (v0): branches and loops, completely

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
