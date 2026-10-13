# Scoping rules (implemented)

Status: 2026-10-13. Companion to `control-flow.md`; this is the invariant set that
makes the rest of the design real.

## The model

- **A scope is any node opened with `{`**: `module`, `func`, CFG `block`, `loop`.
  Membership is structural (`childrenOf`) — no implicit nesting semantics beyond
  containment.
- **Names are scope-local.** A name resolves through the scope chain: innermost
  scope outward, then the document root. Shadowing is allowed; the innermost
  binding wins.
- **Ids are global identity.** They never renumber and are valid anywhere — but
  being *referenceable* is not the same as being *allowed*. That distinction is
  what the reference rule below enforces.
- **Facts are scoped.** An invariant holds where it is written and is visible to
  inner scopes through the same chain (`ask` queries a scope; outside it the honest
  answer is Unknown).

## The reference rule (now enforced)

> A reference must be visible either **lexically** (structured scopes:
> module/func/loop — via the scope chain) or **by CFG dominance** (block scopes:
> the defining block dominates the use). Crossings are allowed only on
> **sanctioned roads**, declared by traits:
>
> - `terminator` may reference **blocks and edges** — the control-flow delivery
>   sites;
> - `edge` may reference **its target block** (operand 0);
> - `phi` may reference **values in predecessor blocks** — that is literally its
>   semantics (the LLVM-fidelity form).
>
> Everything else that crosses a boundary is an error, with a reason: *"is in
> block '...' which does not dominate this use"* or *"crosses a scope boundary;
> pass values through block parameters or edges."*

Two rules, because there are two kinds of visibility:

- **Structured scopes** get lexical scoping — cheap (parent links only), and it
  forbids exactly what the block-parameter discipline exists to prevent.
- **CFG blocks** get **dominance** — the real LLVM rule, computed with a
  textbook iterative dominator pass over the block graph (successors come from
  terminators *and* from `edge`s inside the block). This is why `dot_llvm.br`
  may read a value from the header block inside the body: it dominates.
  `tests/cases/bad_scope.br` (sibling blocks, no path) is rejected.

Intra-block **ordering** is still free (references may point forward); a
dominance-guided ordering check is the next layer.

Negative tests: `bad_scope.br` (no dominance). Legal crossings are covered by
`loop_cfg.br` (edge targets), `branch_cpp.br` (terminator → edges), and
`dot_llvm.br` (phi → predecessor values, plus a dominating header value).

## Deliberately not enforced yet

- **Ordering/dominance.** References may point *forward* inside a scope
  (resolution is order-independent). That keeps textual editing and incremental
  construction cheap; a dominance check is the next structural layer.
- **Symbol visibility.** `func`/`module` carry `@sym` names, but there is no
  private/public/nested visibility or hierarchical `@mod::@fn` resolution yet.
  Modules as symbol roots are the intended home for it.
- **Containment rules.** Nothing states "blocks contain instructions, modules
  contain funcs". That is a schema-level structural constraint — a level-3
  "constraints as data" candidate, not verifier C++.
- **Cross-module references.** `#include` composes textually today; real symbol
  resolution across modules arrives with the symbol table.

## Why this order

1. Identity first (stable ids) — otherwise references cannot survive transforms.
2. Lexical visibility next (this rule) — otherwise the dataflow discipline of
   block arguments is a convention, not an invariant.
3. Dominance, symbol visibility, containment after — each is a *separate*,
   declarable layer, and each should be stated in the schema where possible.
