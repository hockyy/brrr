#!/usr/bin/env bash
# BridgeIR core tests: stable identity, editable names, canonical text, facts,
# schemas (primitives, inheritance, traits), JSON and C-like encodings.
set -u

BIN="${1:?usage: run_tests.sh <bridgeir-binary> <cases-dir>}"
CASES="${2:?usage: run_tests.sh <bridgeir-binary> <cases-dir>}"
fail=0

check_text() { # <name> <expected-file> <command...>
  local name="$1" expected="$2"
  shift 2
  local actual
  actual="$("$@" 2>/dev/null)"
  if [ "$actual" == "$(cat "$expected")" ]; then
    echo "ok: $name"
  else
    echo "FAIL: $name"
    diff <(printf '%s\n' "$actual") "$expected" | head -20
    fail=1
  fi
}

check_text "print canonicalizes input" "$CASES/add.canon.br" \
  "$BIN" print "$CASES/add.bad.br"
check_text "demo mints fresh id, preserves old ids" "$CASES/demo.canon.br" \
  "$BIN" demo "$CASES/add.canon.br"

if "$BIN" roundtrip "$CASES/add.canon.br" >/dev/null 2>&1; then
  echo "ok: roundtrip id-stable"
else
  echo "FAIL: roundtrip id-stable"
  fail=1
fi

if "$BIN" demo "$CASES/add.canon.br" 2>&1 >/dev/null | grep -q "insert n6"; then
  echo "ok: journal records insert n6"
else
  echo "FAIL: journal records insert n6"
  fail=1
fi

if "$BIN" demo "$CASES/add.canon.br" 2>&1 >/dev/null | grep -q "retarget n5"; then
  echo "ok: journal records retarget n5"
else
  echo "FAIL: journal records retarget n5"
  fail=1
fi

# Rename by id: references printed by name follow the rename.
rename_out="$("$BIN" rename "$CASES/add.canon.br" n4 total 2>/dev/null)"
if echo "$rename_out" | grep -q "n4 total = add acc bias beta=1.5" &&
   echo "$rename_out" | grep -q "n5 out = func.return total"; then
  echo "ok: rename by id updates node and references"
else
  echo "FAIL: rename by id"
  fail=1
fi

# Rename by name works too, and the journal records the history.
rename_out="$("$BIN" rename "$CASES/add.canon.br" sum subtotal 2>/dev/null)"
if echo "$rename_out" | grep -q "n4 subtotal = add acc bias beta=1.5"; then
  echo "ok: rename by name"
else
  echo "FAIL: rename by name"
  fail=1
fi

if "$BIN" rename "$CASES/add.canon.br" n4 total 2>&1 >/dev/null \
     | grep -q "rename n4: sum -> total"; then
  echo "ok: journal records rename history"
else
  echo "FAIL: journal records rename history"
  fail=1
fi

# Unknown names are rejected.
if "$BIN" print "$CASES/bad-ref.br" >/dev/null 2>&1; then
  echo "FAIL: unknown name accepted"
  fail=1
else
  echo "ok: unknown name rejected"
fi

# Example mappings of external IRs must round-trip.
EXAMPLES="$(cd "$CASES/../.." && pwd)/examples"
for ex in "$EXAMPLES"/*.br; do
  if "$BIN" roundtrip "$ex" >/dev/null 2>&1; then
    echo "ok: roundtrip example $(basename "$ex")"
  else
    echo "FAIL: roundtrip example $ex"
    fail=1
  fi
done

# First-hand symbols: facts live in the IR document.
if "$BIN" facts "$EXAMPLES/reshape_facts.br" >/dev/null 2>&1; then
  echo "ok: facts apply cleanly from the document"
else
  echo "FAIL: facts apply cleanly from the document"
  fail=1
fi

fold_out="$("$BIN" fold-reshape "$EXAMPLES/reshape_facts.br" 2>&1)"
if echo "$fold_out" | grep -q "isEqual(src_size, dst_size) -> Yes" &&
   echo "$fold_out" | grep -q "aligned(dst_size, 16) -> Yes" &&
   echo "$fold_out" | grep -q "\[journal\] compose" &&
   echo "$fold_out" | grep -q "r_fused"; then
  echo "ok: fold-reshape composes from facts (first-hand queries)"
else
  echo "FAIL: fold-reshape composes from facts"
  echo "$fold_out" | head -8
  fail=1
fi

# Contradictory facts make the document ill-formed: every load path rejects them.
for c in print facts fold-reshape; do
  if "$BIN" "$c" "$CASES/facts_conflict.br" >/dev/null 2>&1; then
    echo "FAIL: contradictory facts accepted by '$c'"
    fail=1
  else
    echo "ok: contradictory facts rejected at load ($c)"
  fi
done

# Schema registry: declared vocabulary is strict; foreign vocabulary is reported.
SCHEMAS="$(cd "$CASES/../schemas" && pwd)"
if "$BIN" check "$EXAMPLES/reshape_facts.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: declared vocabulary validates cleanly"
else
  echo "FAIL: declared vocabulary not clean"
  fail=1
fi

check_out="$("$BIN" check "$EXAMPLES/dot_llvm.br" 2>/dev/null)"
if echo "$check_out" | grep -q "errors: 0" &&
   echo "$check_out" | grep -q "unknown kind"; then
  echo "ok: foreign vocabulary reported as warnings"
else
  echo "FAIL: foreign vocabulary handling"
  fail=1
fi

if "$BIN" check "$CASES/bad_schema.br" >/dev/null 2>&1; then
  echo "FAIL: schema violation accepted"
  fail=1
else
  echo "ok: schema violation rejected"
fi

if "$BIN" print "$CASES/bad_schema.br" >/dev/null 2>&1; then
  echo "FAIL: schema violation accepted at load"
  fail=1
else
  echo "ok: schema violation rejected at load (print)"
fi

# Primitives are declared: unknown scalars are rejected.
if "$BIN" check "$CASES/bad_dtype.br" >/dev/null 2>&1; then
  echo "FAIL: unknown scalar accepted"
  fail=1
else
  echo "ok: unknown scalar rejected (f8 is not a declared primitive)"
fi

# --schemas extends the vocabulary (inheritance from a core kind).
if "$BIN" check --schemas="$SCHEMAS/extra.bs" "$CASES/neg_ok.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: --schemas extends the vocabulary (inheritance)"
else
  echo "FAIL: --schemas extension"
  fail=1
fi

schema_out="$("$BIN" schema 2>/dev/null)"
if echo "$schema_out" | grep -q "kind relu < unary" &&
   echo "$schema_out" | grep -q "scalar f16"; then
  echo "ok: schema command shows inheritance and primitives"
else
  echo "FAIL: schema command"
  fail=1
fi

# JSON encoding: same term model, universal notation.
if "$BIN" to-json "$EXAMPLES/reshape_facts.br" 2>/dev/null \
     | python3 -m json.tool >/dev/null 2>&1; then
  echo "ok: to-json emits valid JSON"
else
  echo "FAIL: to-json validity"
  fail=1
fi
if "$BIN" to-json "$EXAMPLES/reshape_facts.br" 2>/dev/null \
     | grep -q '"kind": "reshape"'; then
  echo "ok: JSON carries nodes and kinds"
else
  echo "FAIL: JSON node encoding"
  fail=1
fi

# C-like view: readable skin, ids preserved as comments.
clike_out="$("$BIN" print --c-like "$EXAMPLES/reshape_facts.br" 2>/dev/null)"
if echo "$clike_out" | grep -q "assume src_size = 512;" &&
   echo "$clike_out" | grep -q "auto r1 = reshape(x, in=src_size, out=mid);" &&
   echo "$clike_out" | grep -q "// n3"; then
  echo "ok: C-like view renders with stable ids as comments"
else
  echo "FAIL: C-like view"
  echo "$clike_out" | head -8
  fail=1
fi

# Modules: the top-level unit and symbol root.
if "$BIN" check "$EXAMPLES/modules.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: modules validate cleanly"
else
  echo "FAIL: module validation"
  fail=1
fi
mod_out="$("$BIN" print "$EXAMPLES/modules.br" 2>/dev/null)"
if echo "$mod_out" | grep -q "module @kernels(n1) {" &&
   echo "$mod_out" | grep -q "  func @reshape_chain(n2) {"; then
  echo "ok: module blocks print with nesting"
else
  echo "FAIL: module printing"
  echo "$mod_out" | head -6
  fail=1
fi

# #include: the rest is just modules.
inc_out="$("$BIN" print "$EXAMPLES/app_includes.br" 2>/dev/null)"
if echo "$inc_out" | grep -q "func @identity(n1) {" &&
   echo "$inc_out" | grep -q "module @kernels(n10) {"; then
  echo "ok: #include expands modules into one document"
else
  echo "FAIL: #include expansion"
  echo "$inc_out" | head -8
  fail=1
fi
if "$BIN" check "$EXAMPLES/app_includes.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: included document validates cleanly"
else
  echo "FAIL: included document validation"
  fail=1
fi
if "$BIN" print "$CASES/cycle_a.br" >/dev/null 2>&1; then
  echo "FAIL: include cycle accepted"
  fail=1
else
  echo "ok: include cycle rejected"
fi

# Control flow: blocks are scopes; br/cbr/phi are declared kinds.
if "$BIN" check "$EXAMPLES/loop_cfg.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: control-flow vocabulary validates cleanly"
else
  echo "FAIL: control-flow validation"
  fail=1
fi
cfg_out="$("$BIN" print "$EXAMPLES/loop_cfg.br" 2>/dev/null)"
if echo "$cfg_out" | grep -q "n5 entry = block {" &&
   echo "$cfg_out" | grep -q "e_body = edge body zero n_val" &&
   echo "$cfg_out" | grep -q "n11 acc = param idx=0" &&
   echo "$cfg_out" | grep -q "n9 = cbr has e_body e_exit"; then
  echo "ok: block-argument loop prints (edges carry passed values)"
else
  echo "FAIL: control-flow printing"
  echo "$cfg_out" | head -10
  fail=1
fi

# Scoped invariants: facts hold where written; ask answers in-scope, unknown out.
ask_in="$("$BIN" ask "$EXAMPLES/loop_cfg.br" body "i <= n_val" 2>/dev/null)"
ask_out="$("$BIN" ask "$EXAMPLES/loop_cfg.br" entry "i <= n_val" 2>/dev/null)"
if echo "$ask_in" | grep -q "\-> Yes" &&
   echo "$ask_out" | grep -q "\-> Unknown"; then
  echo "ok: scoped invariants answer inside the loop, unknown outside"
else
  echo "FAIL: scoped invariants"
  echo "in:  $ask_in"
  echo "out: $ask_out"
  fail=1
fi
facts_out="$("$BIN" facts "$EXAMPLES/loop_cfg.br" 2>/dev/null)"
if echo "$facts_out" | grep -q "\[body\]"; then
  echo "ok: facts command shows scopes"
else
  echo "FAIL: facts scoping"
  fail=1
fi

# Vocabulary is modular: core-only leaves control kinds unknown.
core_only_out="$("$BIN" check --core-only "$EXAMPLES/loop_cfg.br" 2>/dev/null)"
if echo "$core_only_out" | grep -q "errors: 0" &&
   echo "$core_only_out" | grep -q "unknown kind 'block'"; then
  echo "ok: control vocabulary is a module (unknown under --core-only)"
else
  echo "FAIL: module-ness of control vocabulary"
  echo "$core_only_out" | head -6
  fail=1
fi

# Block arguments: edges must match the target block's parameters.
if "$BIN" check "$CASES/bad_edge.br" >/dev/null 2>&1; then
  echo "FAIL: edge arity violation accepted"
  fail=1
else
  echo "ok: edge arity violation rejected"
fi

# Verification asymmetry: edge checking is local; phi needs derived predecessors.
if "$BIN" check "$CASES/bad_edge_use.br" >/dev/null 2>&1; then
  echo "FAIL: edge misuse accepted"
  fail=1
else
  echo "ok: edge misuse rejected (terminators only, exactly once)"
fi
if "$BIN" check "$CASES/bad_phi.br" >/dev/null 2>&1; then
  echo "FAIL: phi with non-predecessor accepted"
  fail=1
else
  echo "ok: phi referencing a non-predecessor rejected"
fi

# Declaration level: the schema states phi's tuple structure; the checker proves it.
if "$BIN" check "$CASES/bad_phi_odd.br" >/dev/null 2>&1; then
  echo "FAIL: odd phi arity accepted"
  fail=1
else
  echo "ok: phi tuple structure proven from the declaration (odd count rejected)"
fi
if "$BIN" schema 2>/dev/null | grep -Fq "(value, block)*"; then
  echo "ok: schema declares phi's tuple structure"
else
  echo "FAIL: schema phi structure"
  fail=1
fi

# Simple structured loops: scope + carried params + loop.yield, exits as ports.
if "$BIN" check "$EXAMPLES/loop_simple.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: simple loop validates cleanly"
else
  echo "FAIL: simple loop validation"
  fail=1
fi
simple_out="$("$BIN" print "$EXAMPLES/loop_simple.br" 2>/dev/null)"
if echo "$simple_out" | grep -q "n5 sum = loop zero n_val {" &&
   echo "$simple_out" | grep -q "n11 = loop.yield more new_acc next_i" &&
   echo "$simple_out" | grep -q "n12 = func.return sum"; then
  echo "ok: simple loop prints (carried params, yield, port exit)"
else
  echo "FAIL: simple loop printing"
  echo "$simple_out" | head -14
  fail=1
fi
if "$BIN" ask "$EXAMPLES/loop_simple.br" sum "i <= n_val" 2>/dev/null \
     | grep -q -- "-> Yes"; then
  echo "ok: simple loop invariants are scoped to the loop"
else
  echo "FAIL: simple loop invariants"
  fail=1
fi
if "$BIN" check "$CASES/bad_loop.br" >/dev/null 2>&1; then
  echo "FAIL: loop yield arity violation accepted"
  fail=1
else
  echo "ok: loop yield arity violation rejected"
fi

# C++-shaped sugar: `for`/`while` lower to loop + params + loop.yield.
if "$BIN" check "$EXAMPLES/loop_cpp.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: C++ for/while sugar validates cleanly"
else
  echo "FAIL: C++ for/while validation"
  fail=1
fi
cpp_out="$("$BIN" print "$EXAMPLES/loop_cpp.br" 2>/dev/null)"
if echo "$cpp_out" | grep -q "n5 sum = loop zero n_val {" &&
   echo "$cpp_out" | grep -q "= loop.yield more new_acc next_i" &&
   echo "$cpp_out" | grep -q "n14 w = loop i {"; then
  echo "ok: for/while lower to loop scopes (carried params + yield)"
else
  echo "FAIL: for/while lowering"
  echo "$cpp_out" | head -12
  fail=1
fi
if "$BIN" ask "$EXAMPLES/loop_cpp.br" sum "0 <= i" 2>/dev/null \
     | grep -q -- "-> Yes"; then
  echo "ok: invariants survive for-loop lowering"
else
  echo "FAIL: for-loop invariants"
  fail=1
fi
if "$BIN" check "$CASES/bad_for.br" >/dev/null 2>&1; then
  echo "FAIL: for-loop missing step accepted"
  fail=1
else
  echo "ok: for-loop missing carried update rejected"
fi

# C++-shaped sugar: `if`/`else` lowers to cbr + blocks + edges + a join block.
if "$BIN" check "$EXAMPLES/branch_cpp.br" 2>/dev/null \
     | grep -q "errors: 0, warnings: 0"; then
  echo "ok: C++ if/else sugar validates cleanly"
else
  echo "FAIL: C++ if/else validation"
  fail=1
fi
br_out="$("$BIN" print "$EXAMPLES/branch_cpp.br" 2>/dev/null)"
if echo "$br_out" | grep -q "= cbr greater" &&
   echo "$br_out" | grep -q "= edge pick a" &&
   echo "$br_out" | grep -q "n5 pick = block {"; then
  echo "ok: if/else lowers to cbr + branch blocks + join block"
else
  echo "FAIL: if/else lowering"
  echo "$br_out" | head -14
  fail=1
fi
if "$BIN" ask "$EXAMPLES/branch_cpp.br" n10 "a >= b" 2>/dev/null \
     | grep -q -- "-> Yes"; then
  echo "ok: branch facts are scoped to their block"
else
  echo "FAIL: branch-scoped facts"
  fail=1
fi
if "$BIN" check "$CASES/bad_if.br" >/dev/null 2>&1; then
  echo "FAIL: mismatched if-branch yields accepted"
  fail=1
else
  echo "ok: mismatched if-branch yields rejected"
fi

# Lexical scoping: references must be visible through the scope chain.
if "$BIN" check "$CASES/bad_scope.br" >/dev/null 2>&1; then
  echo "FAIL: cross-scope reference accepted"
  fail=1
else
  echo "ok: cross-scope reference rejected (lexical scoping + dominance)"
fi
# The sanctioned crossings stay legal: phi -> predecessor values, terminator -> edges.
if "$BIN" check "$EXAMPLES/dot_llvm.br" 2>/dev/null | grep -q "errors: 0"; then
  echo "ok: phi crossings remain legal (dot_llvm)"
else
  echo "FAIL: phi crossings rejected"
  fail=1
fi
# scf.while lowered to CFG: the two-region form becomes a three-block cycle in the
# canonical block-argument form (no phi); the edge/argument arity invariant holds.
if "$BIN" roundtrip "$EXAMPLES/scf_while_cfg.br" >/dev/null 2>&1 \
   && "$BIN" check "$EXAMPLES/scf_while_cfg.br" 2>/dev/null | grep -q "errors: 0, warnings: 0"; then
  echo "ok: scf.while -> CFG cycle (block-argument form)"
else
  echo "FAIL: scf.while -> CFG cycle"
  fail=1
fi

# Showcase tour: every file round-trips and validates cleanly.
SHOWCASE="$(cd "$CASES/../../showcase" && pwd)"
for br in "$SHOWCASE"/*.br; do
  if "$BIN" roundtrip "$br" >/dev/null 2>&1; then
    echo "ok: showcase roundtrip $(basename "$br")"
  else
    echo "FAIL: showcase roundtrip $br"
    fail=1
  fi
  if "$BIN" check "$br" 2>/dev/null | grep -q "errors: 0"; then
    echo "ok: showcase check $(basename "$br")"
  else
    echo "FAIL: showcase check $br"
    fail=1
  fi
done

# Showcase queries: functor and scoped facts.
if "$BIN" ask "$SHOWCASE/02_invariants.br" root "aligned(n, 16)" 2>/dev/null \
     | grep -q -- "-> Yes"; then
  echo "ok: showcase functor query (aligned)"
else
  echo "FAIL: showcase functor query"
  fail=1
fi
if "$BIN" ask "$SHOWCASE/04_control_cfg.br" then "a >= b" 2>/dev/null \
     | grep -q -- "-> Yes"; then
  echo "ok: showcase branch-scoped facts"
else
  echo "FAIL: showcase branch facts"
  fail=1
fi

# Vocabulary lives in compiler modules, not in documents.
if "$BIN" check "$CASES/bad_vocab.br" 2>&1 \
     | grep -q "cannot be declared in a document"; then
  echo "ok: inline vocabulary in a document rejected"
else
  echo "FAIL: inline vocabulary accepted"
  fail=1
fi

exit $fail
