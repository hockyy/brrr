# Brrr v1 — language spec

Status: adopted 2026-10-14; replaces the v0 text format (`nN name = kind ...`).
This file is the single source of truth for the surface language. A change to the
language changes this file first.

## Goals

1. **Readable by people and models.** Close to MLIR/LLVM text, which both already
   know: `%value`, `^block`, `@symbol`, `: type`.
2. **Identity without noise.** Every node has a stable id, but the default view does
   not show ids. Stored states (the run store) always carry them.
3. **Values are the symbols.** A dimension, a fact, or a type refers to an SSA value
   by `%name`. Binding a symbol is defining a value; there is no second namespace.
4. **Few concepts.** Module, func, block, op, value, type, fact, attribute. Nothing
   else is core; vocabulary (kinds, scalars) is schema data.

## One page of v1

```
fact tile = 128                               # module symbol: bound by a root fact

func @scale(%n: index, %x: tensor<[%n, 128], f16>) -> tensor<[2*%n, 64], f16> {
  %y = reshape %x : tensor<[2*%n, 64], f16>   # checker proves 128*%n == 128*%n
  return %y
}

func @sum_to_n(%n: i64) -> i64 {
  %zero = const 0 : i64
  %has = icmp %n, %zero pred=gt
  cond_br %has, ^body(%zero, %n), ^exit(%zero)
^body(%acc: i64, %i: i64):
  fact 0 <= %i <= %n
  %i2 = sub %i, 1                             # %i2 means %i - 1 to the solver
  %acc2 = add %acc, %i
  %more = icmp %i2, %zero pred=gt
  cond_br %more, ^body(%acc2, %i2), ^exit(%acc2)
^exit(%r: i64):
  return %r
}
```

## Lexical

| Token | Meaning |
|---|---|
| `%name`, `%12`, `%12#1` | a value; `%<digits>` is an unnamed value whose name is its id; `#k` selects result k |
| `^name` | a block label |
| `@name` | a symbol (function, module) |
| `nID` | a node id, only at the start of a definition, only in stored form |
| `# ...` | comment to end of line (a `#` inside a `%name#k` token is not a comment) |
| `"..."` | string literal |

## Grammar

```
document := item*
item     := fact | module | func | op
module   := [id] 'module' '@'name '{' item* '}'
func     := [id] 'func' '@'name '(' [arg (',' arg)*] ')' ['->' type (',' type)*] '{' body '}'
arg      := [id] '%'name ':' type
body     := (fact | op)* (label (fact | op)*)*
label    := [id] '^'name ['(' arg (',' arg)* ')'] ':'
op       := [id] [value (',' value)* '='] kind [operand (',' operand)*] attr* [':' type (',' type)*] [region]
          | [id] value '=' 'const' literal [':' type]
operand  := expr                                   # `%x` alone is a value operand
          | '[' expr (',' expr)* ']'               # expression list
          | '^'name ['(' ['%'value (',' '%'value)*] ')']   # successor
region   := '{' body '}'
attr     := key '=' literal
literal  := int | float | 'true' | 'false' | ident | string | '[' literal,* ']'
          | '(' literal ':' type ')'
type     := ident | ident '<' '[' [dim (',' dim)*] ']' ',' ident '>'
dim      := expr | '?'
expr     := int | '%'value | symbol | 'dim' '(' '%'value ',' int ')'
          | expr ('+'|'-'|'*') expr | '-' expr | '(' expr ')'
          | expr ('floordiv'|'ceildiv'|'mod') int     # a positive constant; binds like `*`
fact     := 'fact' expr (relop expr)+ | 'fact' 'aligned' '(' expr ',' int ')'
          | 'fact' 'range' '(' expr ',' int ',' int ')'
relop    := '=' | '==' | '<=' | '>='
```

Notes:

- **Operands, then successors.** `cond_br %c, ^a(%x), ^b(%y)` — a successor carries the
  values for its block's arguments, at the branch site. There is no phi and no edge
  node; a successor's arguments must match the target block's arguments in number.
- **Entry block is implicit.** Ops before the first label belong to the function's
  entry. A region (`{ ... }` after an op) has the same shape: an optional first label
  declares the region's arguments.
- **Results.** `%a, %b = op ...` names each result. An op without results has no `=`.
- **Types** are optional (gradual). An unannotated value gets its type from the
  op's shape rule when there is one; otherwise it is untyped and unchecked.

## Identity

Every definition (func, block, arg, op) has a stable id: allocated once, never
renumbered, never reused within a document's history.

- **Stored form** (`print --ids`, and every state in the run store) writes ids in
  front of definitions: `n4 %y = reshape ...`, `n6 ^body(n7 %acc: i64)`,
  `n1 func @f(n2 %n: index)`. It round-trips exactly.
- **Clean form** (`print`, the default) omits them. Unnamed values print as `%<id>`,
  so they keep identity even here.
- **Parsing clean text** assigns ids: `%<digits>` names take that id; everything else
  gets a fresh id. When the text is an *edit* of a known state (`resume --state`,
  `branch --state`), ids are recovered by matching against that state:
  funcs/modules by `@name`, blocks by label, args by position, named values by name
  within their function, unnamed values by their numeric name, result-less ops by
  (enclosing scope, kind, ordinal). Anything unmatched is new; anything missing was
  deleted. The run store records that diff as the edit's journal.

Names are unique per function (values and labels); module-level names are unique per
module.

## Expressions as values

The solver works on expressions over three kinds of atoms:

| Atom | Written | Bound by |
|---|---|---|
| value | `%n` | its definition (an arg, an op result) — SSA scoping is symbol scoping |
| module symbol | `tile` | a root (or module-level) `fact` that mentions it |
| dimension | `dim(%x, k)`; `?` in `%x`'s own type | `%x`'s type. A `?` is a fresh size with identity: two `nonzero` results have different sizes |

An unbound bare identifier is an error (`unbound symbol 'tlie'`), which also catches
typos.

**How far expressions go.** Each operator added weakens what the solver can
promise, so the language stops at what analyses need to see through:

| Tier | Operators | What the solver decides | |
|---|---|---|---|
| 1 | `+ - *`, integer constants | equality completely (polynomial normal form, so products like `%n * %m` commute); inequalities in the linear fragment and by intervals | yes |
| 2 | `floordiv`, `ceildiv`, `mod` by a positive constant | exact when the inner expression is divisible (`(4 * %n) floordiv 2` is `2 * %n`); otherwise an opaque term with its defining bounds (`c*q <= e <= c*q + c - 1`), which proves tiling facts like `%n <= ceildiv(%n, 32) * 32` | yes |
| 3 | `min`, `max` | piecewise; for tile edges like `min(32, %n - %i)` | later |
| — | division or `mod` by a value, floats, comparisons, loads, calls | undecidable or effectful: these stay ops | never |

There is no `/`: integer division must say how it rounds (C truncates toward
zero; `floordiv` rounds toward -∞; they disagree on negatives).

The "decides" column gives the floor guaranteed by the local rules. With Z3
built in, which is the default ([`solver.md`](solver.md)), tier 1 and tier 2
questions are full linear-integer validity checks: anything Z3 can prove within
its timeout is `Yes`.

**An expression as a value.** `%m = apply %base * 2 floordiv 3` defines a value
whose meaning is the expression: MLIR's `affine.apply`, without a map.

**Soundness for machine integers.** Expressions are exact integers; machine
integers wrap. Signedness is on the type (`iN` signed, `uN` unsigned, `index`
signed), not on the op, so every atom has exactly one reading, and each typed atom
carries its type's range as a fact. A value gets a meaning only when its
arithmetic cannot wrap:

- `index` (and untyped values): always — the stated assumption, as in MLIR's affine dialect;
- `iN` / `uN`: only when the op says so (`add %a, 1 nsw=true`, as in LLVM);
- literals and `dim(...)` never wrap.

Otherwise the value is opaque to the solver: precision is lost, correctness is not.

**Casts.** Inside expressions there are none: `tensor<[%a + %b], f32>` with
`%a: i32, %b: u16` is exact arithmetic, each atom read through its own type.
Between machine values a cast is explicit, as in MLIR: `add %a, %b` on `i32` and
`u16` is an error, and there is no implicit promotion. One `cast` op suffices,
because the source and target types say how; it maps onto MLIR's op-level
signedness at the boundary:

| `cast` | MLIR |
|---|---|
| `i32 -> i64` | `arith.extsi` |
| `u16 -> i32` | `arith.extui` |
| `i64 -> i32` | `arith.trunci` |
| `index <-> iN` / `index <-> uN` | `arith.index_cast` / `arith.index_castui` |

A cast means its operand exactly when every source value fits the target
(`i32 -> i64`); otherwise it may wrap and is opaque. `unrealized_cast` is
MLIR's `builtin.unrealized_conversion_cast`: a placeholder between type systems
during an import or a lowering, opaque to the solver, folded by `reconcile-casts`
when it cancels, and a warning (an error under `--strict`) while it remains.

**Dimensions are non-negative.** For an argument (or a block argument, or an op's
own fresh `?`) this is assumed — whoever made the tensor made a real one — so a
tensor type teaches the solver something: inside `@f(%x: tensor<[%a + %b], f32>)`,
`%a + %b >= 0` holds. For every dimension an op produces it is proven: a negative
one is an error, an unproven one a warning (and a `guard ... rel=le` under
`insert-guards`).

**Values have meanings.** A kind may declare, as schema data, what its result means
symbolically:

```
kind add   { operands: lhs, rhs   expr: lhs + rhs }
kind sub   { operands: lhs, rhs   expr: lhs - rhs }
kind mul   { operands: lhs, rhs   expr: lhs * rhs }
kind const { expr: @value }                      # the attribute named `value`
kind dim   { operands: source     expr: dim(source, @index) }
```

So `%i2 = sub %i, 1` makes `%i2` equal to `%i - 1` for every query, and
`%k = dim %x index=0` is the first dimension of `%x`'s type — no new symbol needed.
Values whose kind has no `expr` (args, loads, calls) are atoms. A kind may also fix
its result type (`type: index` for `dim`, `type: i1` for `icmp`).

**Shape rules are data.** A kind may declare how operand shapes relate:

```
kind add     { shape: S, S -> S }            # S = a whole shape
kind matmul  { shape: [M, K], [K, N] -> [M, N] }
kind nonzero { shape: [N] -> [?] : index }   # a fresh size per op; `: index` = element type
kind reshape { traits: same_numel }          # result type is written; numel must match
```

Each repeated variable is a question for the solver, with three outcomes:

- **Yes** — proven for every input: accepted.
- **No** — false for every input: an error that shows the equation (`128 == 100 is false`).
- **Unknown** — depends on the input: a warning naming the equation. Add a fact,
  or run `insert-guards` to turn it into a runtime `guard`; below a guard the
  equation is a fact. `check --strict` makes every Unknown an error.

So each shape claim is either proven at compile time or checked at run time.

## Facts

```
fact tile = 128                 # module symbol
fact aligned(%n, 16)
fact 0 <= %i <= %n              # chains are conjunctions
fact range(%i, 0, 1023)
```

A fact holds in the scope it is written in (module, function, block, region) and in
every scope nested inside it. Where it is written decides who must prove it:

| Where | Role | Proven by |
|---|---|---|
| module level | assumption (configuration) | trusted |
| function, about arguments | precondition | the caller (inside the function: trusted) |
| block, about block arguments | invariant | every edge into the block, with the values it passes |
| about an op result | lemma | the facts and meanings that precede it |

Invariants are checked one edge at a time, which is what removing phi buys: the
values an edge delivers sit at the branch, so substitute and prove in the source
block. A branch condition is a fact on its edge (the true edge of
`cond_br %c` with `%c = icmp %i2, 0 pred=gt` knows `%i2 >= 1`). A loop's entry
block is reached by its initial values and by its back edge, so a loop invariant is
proven by induction.

## Operands, expressions, attributes — the exact split

Everything an op consumes is one of three things, decided by one question each:

| | Can it depend on runtime values? | Written | Example |
|---|---|---|---|
| **value operand** | it *is* a runtime value | `%x` | `add %acc, %i` |
| **expression operand** | yes, through the values it mentions | an integer expression | `sub %i, 1` · `extract_slice %t, [%off, 0], [4, %sz]` |
| **attribute** | never — fixed when the op is written | `key=literal` | `pred=gt` · `align=4` · `to=f16` |

- An expression operand is an integer term over values, module symbols and
  constants (the same `expr` as in types and facts). A constant (`4`) is the static
  case, a value (`%sz`) the dynamic case, and `2*%n + 1` is both — no flag, no
  sentinel, no separate static/dynamic lists. A `[...]` list operand holds several.
- Folding is substitution: when the solver proves `%sz == 4`, the operand `%sz`
  becomes `4`. Nothing changes type.
- The values an expression mentions are ordinary uses: they must dominate the op,
  and use-def queries see them.
- Lowering materializes expressions into arithmetic ops when a backend needs plain
  values; the inverse pass absorbs index arithmetic back into expressions.

What this replaces in MLIR:

| MLIR | Brrr |
|---|---|
| `OpFoldResult` (`Attribute` or `Value`) | expression operand |
| static/dynamic operand pairs + `ShapedType::kDynamic` | one expression list |
| `AffineMap` / `affine.apply` | expressions (polynomial, not only affine) |
| `TypeAttr` | `type` attribute literal |
| `IntegerAttr` / `StringAttr` / enum attrs | attribute literals |
| `DenseElementsAttr` | the literal of a `const` |
| `SymbolRefAttr` | `@name` |

Attributes are plain literal data: no uniquing, no attribute type hierarchy, nothing
to register in C++.

## Attributes

An attribute configures an op. It never describes the type or shape of a value —
that is the value's type.

```
%c = icmp %a, %b pred=gt               # enum (schema: pred: enum(eq, ne, lt, le, gt, ge))
%v = llvm.load %p align=4              # int
%t = transpose %x perm=[1, 0, 2]       # array
%y = cast %x to=f16                    # a type (schema: to: type)
%z = scale %x eps=(1e-5 : f32)         # explicit literal type, for foreign ops
```

Schema attribute types: `int`, `float`, `bool`, `str`, `type`, `enum(a, b, ...)`,
`array`. With a schema, the short form is checked against the declaration; the
explicit `(literal : type)` form is only needed when there is no schema to infer from.

## MLIR interop: affine maps

MLIR's affine expressions are Brrr expressions over `d`/`s` atoms (same
operators, same precedence), so the bridge is exact both ways:

- **Import** (`from-affine`): parse the map, substitute its operands.
  `affine_map<(d0)[s0] -> (d0 * 4 + s0, d0 floordiv 2)>` on `(%i)[%n]` is
  `%i * 4 + %n, %i floordiv 2`.
- **Export** (`to-affine`): every operand position lands on one side of MLIR's
  `OpFoldResult`, decided by the solver: a constant becomes a static attribute
  (`apply %base * 2 floordiv 3` with `fact %base = 6` exports as `4`), a plain value
  stays a value, and anything else becomes `affine.apply` with a map. Atoms split
  into symbols (arguments and values computed at function entry: fixed for the whole
  body) and dims (loop-carried and block values). A product of two values is not
  affine; export refuses it by name, and it goes out as arith ops instead.
- `dim(%x, k)` operands become `tensor.dim`/`memref.dim` on the MLIR side; module
  symbols must be pinned to a constant by facts, since MLIR has no symbol that is not
  an SSA value.

## Removed from v0

| v0 | v1 | Why |
|---|---|---|
| `nN` on every line | ids in stored form only | readability; identity is kept by the store and by matching |
| `param idx=k` nodes | header args `(%x: T)` | one way to declare an argument |
| `edge` nodes | successor arguments on the branch | no sync between edge and branch, no special scoping rule |
| `type="i64"`, `shape=`, `dtype=`, `in=`, `out=` | `: type` | types are checked; attributes are configuration |
| `loop.N` ports | `%a, %b = loop ...` | results are named like any other |
| C++ sugar (`for`/`while`/`if`) | removed | it hid SSA behind mutation (`i = next`); may return as a view |
| C-like view (`print --c-like`) | removed | it existed because v0 text was hard to read; the clean form is the readable view |
| attribute types `dtype`, `dims`, `expr`, `sym` | `int float bool str type array enum(..) any` | shapes are types; expressions are operands |
| `#include` | kept, unchanged | |
