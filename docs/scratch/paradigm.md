# Paradigm and encodings: procedures, objects, primitives, mini-C++

> **2026-10-14:** written for the v0 text format (`nN name = kind ...`, edge and
> param nodes). The current language is [`syntax-v1.md`](syntax-v1.md); the
> reasoning here still stands where v1 does not say otherwise.

Status: recorded 2026-10-13, refined over the session. Answers four questions that
shaped the design.

## "A short data structure that can hold many interpretations?"

A **tagged term**: `{tag, fields...}`, recursively. It is the shortest thing that
can mean anything, because meaning comes from the schema, not the shape. The IR is
built from it everywhere: `Expr = {name, args}`, `TypeTerm = {name, args}`,
`Node = {id, kind, operands, attrs, scope}`.

**JSON is a notation for it — and it is "almost perfect"** in that role: parsers in
every language, agents know it cold, diffable, schema-checkable. Limits:
canonicalization (key order/whitespace must be normalized before hashing), no
symbol primitive (we encode `{"type": "sym", ...}`), and verbosity.

So encodings are interchangeable; semantics live once:

| Encoding | Role |
|---|---|
| canonical line text (`.br`) | surface + **the parse format**: token-lean, diff-stable |
| JSON (`to-json`) | wire/interchange: connectors, other languages |
| C-like view (`print --c-like`) | readable skin for humans/agents (implemented) |
| versioned binary (planned) | speed: caches, large modules |

## "How do you define a return type?"

Two layers, both data:

1. **Results policy** (schema): `results: value` vs `results: none` — does the kind
   produce a value at all? Enforced: a `results: none` node may not be referenced.
2. **Typing signature** (`types::Signature`): the *what*, as terms with variables —
   `mm : tensor<A, M, K> x tensor<A, K, N> -> tensor<f32, M, N>`. Inference
   instantiates per node and unifies; mismatches report the failing equation.
   Next: carry `signature:` in the kind definition.

## Procedural + object-shaped data (not Java-OO)

The user's refinement, adopted: **procedural core + objects as data**.

- **Procedures**: passes, rules, and pipelines are procedures over modules; a
  pipeline is a sequence of procedures; behaviour is data that composes.
- **Objects, lightly**: nodes are records with identity (`id`), a class tag
  (`kind`), and fields (`attrs`); schemas are their class definitions.
- **Inheritance is flat field sharing** (`kind relu < unary`), traits are interface
  tags — not a dispatch mechanism. There is no method resolution on objects, no
  encapsulation, no `this`.
- **Why not full OO**: methods-on-objects couple behaviour to data (MLIR's ODS
  lesson); data-first keeps extension cheap for agents — adding a class or a rule
  is adding data, never a rebuild.

Functional ideas live in the *query stratum*, not the object model: facts are
equations, inference is unification, rewrites are selectors + templates; connectors
(SymPy/Z3) are its engines.

## Primitive data types

Primitives are **declared**, not assumed:

```
scalar f16 { domain: float bits: 16 }
scalar i32 { domain: int bits: 32 }
```

Consequences: `dtype=@f8` is rejected ("unknown scalar"); inference can use domains;
the symbolic layer can later range-check constants against `bits`/`domain`; and
`brrr schema` prints the vocabulary. Primitives are part of the language's
definition in exactly the same way kinds are.

## Mini-C++ as the surface?

Half in jest, wholly in earnest: **agents are C++-fluent.** A C-like surface is
high-leverage for token economy and fluency, and it matches procedural + objects:

```
assume src_size = 512;
assume aligned(dst_size, 16);

reshape_chain() {
  auto x = load(dtype=@f16, shape=src_size);       // n2
  auto r1 = reshape(x, in=src_size, out=mid);      // n3
  auto r2 = reshape(r1, in=mid, out=dst_size);     // n4
  return r2;                                       // n5
}
```

Rules of engagement, so it stays an asset:

- It is a **view/encoding, not semantics** — same nodes, same stable ids (shown as
  `// nID` comments), same facts. Canonical `.br` remains the parse format for now.
- We keep a clean subset: no template angle-bracket parsing (the one thing we will
  *not* inherit from C++), no UB, no header/declaration split.
- When it becomes parseable, the mapping is direct: `T name = kind(args)` → node;
  `return` → `func.return`; scopes → functions; `assume` → facts; schema `kind`
  blocks already *are* struct-like definitions.

## Core runner + modules: "the rest is just modules"

The architecture, made literal:

- **Main head** — the `brrr` CLI: a thin shell.
- **Parser** — text → terms (canonical `.br`, schemas, facts, `#include`).
- **Runner / executor** — procedures over modules: passes, inference, the solver,
  connectors.
- **Everything else is modules**: IR fragments compose via `#include <...>` /
  `#include "..."` (cycle-checked, depth-limited, `--include=<dir>`); vocabulary
  composes via schema files (`--schemas=`); engines compose via connectors (SymPy,
  later Z3); passes compose via plugins. Structs are kinds, functions are funcs,
  includes are includes.

One sentence: **C-like where it helps humans and agents read, procedural where it
helps the compiler act, object-shaped data where it helps extension.**
