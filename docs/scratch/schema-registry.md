# Schema registry: the language's definition (v0)

Status: implemented, 2026-10-13. Answers the "the IR is too vague" critique: node
kinds are no longer bare strings — they are declared as data with **named ports**,
a **results policy**, and **typed attributes**, and every load validates them.

## Grammar (v0)

```
# comments
kind reshape {
  operands: source            # positional & named: `none` | `a, b` | `tail*` (variadic)
  results: value              # `value` (default) | `none` (terminator)
  attrs: in:dims out:dims     # omitted => free | `none` | closed set of name:type
}
```

Attribute types: `int`, `float`, `str`, `sym`, `expr` (one expression), `dims`
(comma-separated expressions, e.g. shapes). `expr`/`dims` strings are parsed with
the language's expression parser **at load** — a malformed shape is a document
error, not a pass-time crash.

## Policies

- **Declared kinds are strict**: operand counts, required attributes, attribute
  types. A kind that `results: none` may not be referenced as an operand.
- **Foreign kinds are warnings**: imported vocabulary (`llvm.*`, `hivm.*`,
  FX names) is expected in a multi-IR framework, but it is *reported* — nothing is
  silently accepted. `bridgeir check` lists everything.
- **Core vocabulary (v0)**: `func`, `param`, `load`, `reshape`, `add`, `relu`,
  `func.return`. Extend with `--schemas=<file>` (repeatable).

## CLI

- `bridgeir check <file>` — full report, then `errors: E, warnings: W`; exit 1 on
  errors.
- **Every other command validates before running**: an ill-formed document never
  reaches a pass.

## Why this is the "not vague" fix

Before: `n4 = reshape r1 in="mid" out="dst_size"` was only interpretable by reading
`fold_reshape.cpp`, and `in=`/`out=` were conventions the pass happened to expect.
Now the kind is *defined*: port `source` (arity 1), attributes `in`/`out` of type
`dims` (parse-checked expressions). A checker, an agent, or a refactoring tool can
read the definition instead of reverse-engineering pass code.

## Inheritance, traits, scalars (v1)

```
kind unary { operands: x results: value traits: pure }
kind relu < unary { }                # inherits ports/results/traits
scalar f16 {                         # primitives are declared, not assumed
  domain: float
  bits: 16
}
```

- `kind X < Parent` copies unspecified fields (ports, results, attr policy,
  traits); children override by specifying a field. Parent must already be
  declared.
- `traits` are interface tags for rules/checkers to dispatch on.
- Attr type `dtype` validates a symbol against declared scalars — `dtype=@f8`
  errors ("unknown scalar"), which is what makes primitives part of the language
  definition rather than naming conventions.
- `bridgeir schema` prints the resolved vocabulary (scalars + kinds, core +
  `--schemas`).

## Modules

`module @name(mID) { func ... }` is the top-level unit and symbol root; documents
without one use the implicit root. Name resolution walks the scope chain
(func → module → root). Modules are the natural serialization/exchange boundary
for connectors and importers.

## Mini-C++ surface (view)

`bridgeir print --c-like` renders the module in a C-like style for fluent reading:
facts become `assume ...;`, nodes become typed declarations
(`auto r1 = reshape(x, in=src_size, out=mid); // n3`), returns become `return ...;`,
and modules read as `namespace`. View-only; the canonical `.br` text remains the
parse format. A parseable C-like dialect is the candidate third encoding.

## Future

- Versioned schema files with content hashes; documents pin their vocabulary.
- Type terms (not strings) in `type:` slots; `dims` attrs as structured data.
- Per-kind fact rules (schema-generated facts for `split`/`pad`/`mm`) and verifier
  hooks as schema fields.
- C-like surface as a *parseable* alternate encoding (ids, facts, scopes preserved).
