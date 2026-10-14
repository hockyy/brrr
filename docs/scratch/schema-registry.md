# Schema registry: the vocabulary as data (v1)

Status: v1, 2026-10-14. Replaces the v0 grammar (port types `value|block|edge`,
`(value, block)*` groups, `dims`/`dtype`/`expr` attribute types), which described
edge nodes and stringly shapes that no longer exist.

A kind declares everything a checker, the solver, or a pass needs to know about
it. No kind needs C++ to be understood; a new op is a few lines of data.

## Grammar

```
scalar f16 {
  domain: float          # int | uint | float | bool | index | ptr
  bits: 16
}

kind matmul {
  operands: lhs, rhs                  # named, positional; `rest*` = variadic tail; `none`
  successors: 0                       # how many ^block targets (`*` = any); default 0
  results: value                      # none | value | value* | a count; default value
  attrs: pred: enum(eq|ne|gt), align?: int   # free (default) | none | closed list; `?` = optional
  regions: 0                          # ops with a `{ ... }` region declare 1
  type: index                         # a fixed result type (dim: index, icmp: i1)
  expr: lhs * rhs                     # what result 0 means to the solver
  shape: [M, K], [K, N] -> [M, N]     # how operand shapes relate
  traits: pure                        # interfaces: pure, terminator, loop, same_numel, ...
}

kind relu < unary {                   # inherits every field it does not set
}
```

Attribute types: `int`, `float`, `bool`, `str`, `type`, `array`, `any`,
`enum(a|b|...)`. Attributes configure an op; they never carry a value's type or
shape (see the "exact split" in `syntax-v1.md`).

## `expr:` — meanings

A template over operand names and attributes:

| Template | Meaning |
|---|---|
| `lhs + rhs`, `lhs - rhs`, `lhs * rhs` | arithmetic on the operands (values or expressions) |
| `@value` | an integer attribute (`const`) |
| `dim(source, @index)` | a dimension of the operand's type |
| `formula` | the operand itself (`apply`, whose operand is an expression) |

A meaning is used only when the result cannot wrap: always for `index`, and for
`iN`/`uN` only with `nsw=true`/`nuw=true` on the op (see `syntax-v1.md`).

## `shape:` — shape rules

`P1, P2, ... -> R [: elem]`, where a pattern is either a whole-shape variable
(`S`) or a dimension list (`[M, 128, ?]`). Uppercase names are variables; integers
are literals; `?` in the result is a fresh dimension owned by the op. Every
repeated variable is an equation for the solver (Yes / No / Unknown). An
elementwise rule (`S, S -> S`) also types scalar operands. `: index` names the
result's element type; otherwise it is the first operand's (or the `to` attribute's
for a conversion).

## The standard modules

`core` (scalars, `return`), `std` (`const`, `apply`, `add`/`sub`/`mul`, `icmp`,
`select`, `dim`, `relu`, `cast`, `reshape`, `matmul`, `nonzero`, `guard`),
`control` (`br`, `cond_br`, `loop`, `continue_if`). `brrr schema` prints them;
`--schemas=<file>` adds kinds; `--core-only` drops `std` and `control`. A kind no
loaded schema declares is foreign: accepted, reported once as a warning, and not
checked.
