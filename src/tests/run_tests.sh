#!/usr/bin/env bash
# Brrr CLI tests (v1).
#   * every showcase file and example verifies and round-trips (both forms)
#   * every case in tests/cases with an `# expect: <text>` line fails `check`
#     with that text (warn_* cases are warnings: they fail only with --strict)
#   * identity, facts, shapes, JSON, schema, and the run store behave as documented
set -u

BIN="${1:?usage: run_tests.sh <brrr-binary> <cases-dir>}"
CASES="${2:?usage: run_tests.sh <brrr-binary> <cases-dir>}"
SRC="$(cd "$CASES/../.." && pwd)"
SHOWCASE="$SRC/showcase"
EXAMPLES="$SRC/examples"
SCHEMAS="$SRC/tests/schemas"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

pass() { echo "ok: $1"; }
flunk() {
  echo "FAIL: $1"
  [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/    /'
  fail=1
}
# contains <name> <needle> <command...>: stdout+stderr of the command contains needle
contains() {
  local name="$1" needle="$2"
  shift 2
  local out
  out="$("$@" 2>&1)"
  if printf '%s' "$out" | grep -qF -- "$needle"; then pass "$name"; else flunk "$name" "$out"; fi
}
# status <name> <expected-exit> <command...>
status() {
  local name="$1" want="$2"
  shift 2
  "$@" >/dev/null 2>&1
  local got=$?
  if [ "$got" -eq "$want" ]; then pass "$name"; else flunk "$name" "exit $got, expected $want"; fi
}

# ---- every showcase file and example verifies and round-trips ----------------
for f in "$SHOWCASE"/*.br "$EXAMPLES"/*.br; do
  status "verifies: ${f#$SRC/}" 0 "$BIN" check "$f"
  status "round-trips: ${f#$SRC/}" 0 "$BIN" roundtrip "$f"
done

# ---- negative cases: each fails with the message it declares -----------------
for f in "$CASES"/*.br; do
  expect="$(sed -n 's/^# expect: //p' "$f")"
  [ -z "$expect" ] && continue
  name="$(basename "$f")"
  contains "$name reports: $expect" "$expect" "$BIN" check "$f"
  case "$name" in
    warn_*)
      status "$name passes without --strict" 0 "$BIN" check "$f"
      status "$name fails with --strict" 1 "$BIN" check --strict "$f"
      ;;
    *) status "$name is rejected" 1 "$BIN" check "$f" ;;
  esac
done

contains "no-wrap: nsw=true gives i32 arithmetic a meaning" "errors: 0, warnings: 0" \
  "$BIN" check "$CASES/nsw_i32.br"
contains "schema extension: --schemas declares a kind" "errors: 0, warnings: 0" \
  "$BIN" check --schemas="$SCHEMAS/extra.bs" "$CASES/neg_ok.br"
contains "schema extension: without it the kind is foreign" "unknown kind 'neg'" \
  "$BIN" check "$CASES/neg_ok.br"
contains "include cycles are rejected" "include cycle" "$BIN" print "$CASES/cycle_a.br"

# ---- PyTorch in: examples/kernel.py exports a kernel, Brrr re-proves its shapes --------
TORCH_EX="$SRC/../examples"
contains "pytorch: the exported kernel verifies" "errors: 0, warnings: 0" \
  "$BIN" check "$TORCH_EX/kernel.br" --schemas="$TORCH_EX/aten.bs"
contains "pytorch: torch's 8 * Dim(t) stays an expression" "%x: tensor<[8 * %t, 64], f32>" \
  "$BIN" print "$TORCH_EX/kernel.br"
contains "pytorch: torch's range is a fact" "fact 2 <= %t <= 512" "$BIN" print "$TORCH_EX/kernel.br"
sed 's/%fc2_bias: tensor<\[64\], f32>/%fc2_bias: tensor<[32], f32>/' "$TORCH_EX/kernel.br" > "$TMP/kernel_bad.br"
contains "pytorch: a wrong shape is caught by Brrr's own rules" "64 == 32 is false" \
  "$BIN" check "$TMP/kernel_bad.br" --schemas="$TORCH_EX/aten.bs"
TORCH_PY=""
for p in python3 python; do
  if "$p" -c "import torch" >/dev/null 2>&1; then TORCH_PY="$p"; break; fi
done
if [ -n "$TORCH_PY" ]; then
  mkdir -p "$TMP/torch"
  "$TORCH_PY" -I "$TORCH_EX/kernel.py" "$TMP/torch" >/dev/null 2>&1
  if cmp -s "$TMP/torch/kernel.br" "$TORCH_EX/kernel.br" && cmp -s "$TMP/torch/aten.bs" "$TORCH_EX/aten.bs"; then
    pass "pytorch: rerunning kernel.py reproduces the committed files"
  else
    flunk "pytorch: rerunning kernel.py reproduces the committed files" \
      "$(diff "$TMP/torch/kernel.br" "$TORCH_EX/kernel.br" | head)"
  fi
else
  echo "skip: no python with torch (kernel.py regeneration)"
fi

# ---- identity --------------------------------------------------------------------
contains "clean form: an unnamed value prints as its id" "%5 = relu %sum" \
  "$BIN" print "$SHOWCASE/01_identity.br"
contains "stored form: definitions carry ids" "n5 %5 = relu %sum" \
  "$BIN" print --ids "$SHOWCASE/01_identity.br"
"$BIN" print --ids "$SHOWCASE/04_cfg.br" > "$TMP/stored.br" 2>/dev/null
if [ "$("$BIN" print --ids "$TMP/stored.br" 2>/dev/null)" == "$(cat "$TMP/stored.br")" ]; then
  pass "stored form re-parses to itself (ids kept)"
else
  flunk "stored form re-parses to itself (ids kept)"
fi
contains "rename updates the definition" "%total = add %x, %y" \
  "$BIN" rename "$SHOWCASE/01_identity.br" %sum %total
contains "rename updates every use" "%5 = relu %total" \
  "$BIN" rename "$SHOWCASE/01_identity.br" %sum %total
contains "rename is journaled" "[journal] rename" \
  "$BIN" rename "$SHOWCASE/01_identity.br" %sum %total

# ---- facts and queries -------------------------------------------------------------
contains "ask: a definition is a fact" "-> Yes" "$BIN" ask "$SHOWCASE/02_facts.br" @split "%n == 128 * %tiles"
contains "ask: module facts" "-> Yes" "$BIN" ask "$SHOWCASE/02_facts.br" root "aligned(tile, 16)"
contains "ask: a value means its expression" "-> Yes" "$BIN" ask "$SHOWCASE/04_cfg.br" ^body "%i2 + 1 == %i"
contains "ask: loop invariant in scope" "-> Yes" "$BIN" ask "$SHOWCASE/05_loop.br" ^iter "%i2 <= %n"
contains "ask: honest Unknown" "-> Unknown" "$BIN" ask "$SHOWCASE/06_shapes.br" @scores "%b <= %s"
contains "facts lists facts by scope" "[^body] fact 0 <= %i <= %n" "$BIN" facts "$SHOWCASE/04_cfg.br"
sed 's/^  fact %n >= 0$//' "$SHOWCASE/05_loop.br" > "$TMP/no_pre.br"
contains "induction: the invariant needs the precondition on entry" \
  "invariant 0 <= %i of ^iter on loop entry: 0 <= %n is not proven" "$BIN" check "$TMP/no_pre.br"
sed 's/%more = icmp %i2, %zero pred=gt/%more = icmp %i2, %zero pred=ne/' "$SHOWCASE/04_cfg.br" > "$TMP/ne.br"
contains "branch conditions: pred=ne no longer proves the back edge" \
  "from ^body (true edge): 0 <= %i2 is not proven" "$BIN" check "$TMP/ne.br"

# ---- shapes and meanings -------------------------------------------------------
contains "types: matmul infers [M, N]" "%scores = matmul : tensor<[%b * %s, %s * %b], f16>  (inferred)" \
  "$BIN" types "$SHOWCASE/06_shapes.br"
contains "types: a fresh dimension prints as ?" "%idx = nonzero : tensor<[?], index>" \
  "$BIN" types "$SHOWCASE/06_shapes.br"
contains "types: dim means a dimension" "%c = dim : index  (inferred)  means dim(%idx, 0)" \
  "$BIN" types "$SHOWCASE/06_shapes.br"
contains "types: index arithmetic has a meaning" "%i2 = sub : index  (inferred)  means %i - 1" \
  "$BIN" types "$SHOWCASE/04_cfg.br"

# ---- division and the affine bridge ------------------------------------------------
AFF="$SHOWCASE/09_affine.br"
contains "ceildiv covers its input" "-> Yes" "$BIN" ask "$AFF" @tiles "%n <= %count * 32"
contains "mod is in [0, c)" "-> Yes" "$BIN" ask "$AFF" @tiles "0 <= %lane <= 31"
contains "an exact division simplifies" "-> Yes" "$BIN" ask "$AFF" @tiles "(4 * %n) floordiv 2 == 2 * %n"
printf 'func @f(%%a: index) -> index {\n  %%b = apply %%a / 3\n  return %%b\n}\n' > "$TMP/slash.br"
contains "/ is rejected: division must say how it rounds" "use floordiv or ceildiv" "$BIN" check "$TMP/slash.br"
contains "to-affine: a constant is a static attribute" "static 4" "$BIN" to-affine "$AFF" @pinned "%mult"
contains "to-affine: an apply exports as affine.apply" \
  "defined as affine.apply affine_map<()[s0] -> ((s0 * 2) floordiv 3)>()[%base]" \
  "$BIN" to-affine "$AFF" @tiles "%mult"
contains "to-affine: function values are symbols" \
  "affine.apply affine_map<()[s0, s1] -> (s0 * 4 + s1)>()[%n, %base]" \
  "$BIN" to-affine "$AFF" @tiles "%n * 4 + %base"
contains "to-affine: loop values are dims" "affine_map<(d0)[s0] -> (d0 * 4 + s0)>(%i)[%n]" \
  "$BIN" to-affine "$SHOWCASE/05_loop.br" ^iter "%i * 4 + %n"
contains "to-affine: a product of values is refused, by name" \
  "not affine: %n * %base multiplies two non-constant terms" \
  "$BIN" to-affine "$AFF" @tiles "%n * %base"
contains "from-affine: substitutes dims and symbols" "%i * 4 + %n, %i floordiv 2" \
  "$BIN" from-affine "affine_map<(d0)[s0] -> (d0 * 4 + s0, d0 floordiv 2)>" %i %n
contains "from-affine: rejects names the map does not declare" "is not a dim or symbol" \
  "$BIN" from-affine "affine_map<(d0) -> (d0 * d1)>" %i

# ---- casts and signedness ------------------------------------------------------------
X="$CASES/cross_signed.br"
contains "cross-signed dims are exact arithmetic" "errors: 0, warnings: 0" "$BIN" check "$X"
contains "a tensor argument's dims are non-negative" "-> Yes" "$BIN" ask "$X" @f "%a + %b >= 0"
contains "cast: to= sets the scalar type" "%wide = cast : i64  (inferred)  means %a" "$BIN" types "$X"
contains "cast: a widening means its operand" "-> Yes" "$BIN" ask "$X" @f "%wide == %a"
contains "cast: a sign change may wrap, so it is opaque" "-> Unknown" "$BIN" ask "$X" @f "%u == %a"
contains "reconcile-casts folds a round trip" "status: done" \
  "$BIN" run "$CASES/warn_unrealized_cast.br" --passes=reconcile-casts,dce "--store=$TMP/casts"
contains "after reconcile-casts nothing remains" "  return %x" "$BIN" show r1 "--store=$TMP/casts"

# ---- the solver backend --------------------------------------------------------------
if "$BIN" passes 2>/dev/null | grep -q "solver: z3"; then
  # check mode asks the local rules and Z3 every question; any disagreement is a bug.
  disagreements=""
  for f in "$SHOWCASE"/*.br "$EXAMPLES"/*.br "$CASES"/*.br; do
    d="$(BRRR_SOLVER=check "$BIN" check "$f" 2>&1 >/dev/null | grep "solver check:")"
    [ -n "$d" ] && disagreements="$disagreements$f: $d"$'\n'
  done
  if [ -z "$disagreements" ]; then
    pass "solver check: local rules and Z3 agree on every file"
  else
    flunk "solver check: local rules and Z3 agree on every file" "$disagreements"
  fi
  printf 'fact tile = 32\nfunc @f(%%n: index, %%i: index, %%x: tensor<[%%n], f32>) -> index {\n  fact %%n = 4 * tile\n  %%a = apply %%i * 4 + %%n - %%n\n  %%b = apply (%%n floordiv 32) * tile\n  %%c = apply (2 * %%i + 6) floordiv 2\n  %%y = reshape %%x : tensor<[%%n * 1 + 0], f32>\n  return %%a\n}\n' > "$TMP/canon.br"
  contains "canonicalize runs" "status: done" "$BIN" run "$TMP/canon.br" --passes=canonicalize "--store=$TMP/canon"
  contains "canonicalize: algebra" "%a = apply 4 * %i" "$BIN" show r1 "--store=$TMP/canon"
  contains "canonicalize: facts give constants" "%b = apply 128" "$BIN" show r1 "--store=$TMP/canon"
  contains "canonicalize: exact division" "%c = apply 3 + %i" "$BIN" show r1 "--store=$TMP/canon"
  contains "canonicalize: result dims too" "reshape %x : tensor<[128], f32>" "$BIN" show r1 "--store=$TMP/canon"
  contains "canonicalize: the signature stays as written" "%x: tensor<[%n], f32>" "$BIN" show r1 "--store=$TMP/canon"
else
  echo "skip: built without Z3 (solver checks and canonicalize)"
fi

# ---- encodings and vocabulary ----------------------------------------------------
PY=python3
python3 -c '' >/dev/null 2>&1 || PY=python
if "$BIN" to-json "$SHOWCASE/06_shapes.br" 2>/dev/null | "$PY" -m json.tool >/dev/null 2>&1; then
  pass "to-json emits valid JSON"
else
  flunk "to-json emits valid JSON"
fi
contains "to-json is version 1" '"version": 1' "$BIN" to-json "$SHOWCASE/01_identity.br"
contains "schema prints shape rules" "shape: [M, K], [K, N] -> [M, N]" "$BIN" schema
contains "schema prints meanings" "expr: lhs - rhs" "$BIN" schema

# ---- the run store ---------------------------------------------------------------
STORE="--store=$TMP/store"
contains "run: a pipeline file runs to the end" "status: done" \
  "$BIN" run "$SHOWCASE/08_pipeline.br" --pipeline="$SHOWCASE/08_pipeline.txt" "$STORE"
contains "show: the final state" "%y = relu %x : tensor<[%n, %m], f32>" "$BIN" show r1 "$STORE"
contains "show: any stage" "%flat = reshape %x" "$BIN" show r1@1 "$STORE"
contains "show --record: the stage's journal" "journal fold" "$BIN" show r1@2 --record "$STORE"
contains "branch: a new run from a stage" "run r2 (from r1@1)" "$BIN" branch r1@1 --passes=dce "$STORE"
contains "runs: lists parents" "r2  done  2 stage(s)  from r1@1" "$BIN" runs "$STORE"
contains "run --steps pauses" "status: paused after stage 1" \
  "$BIN" run "$SHOWCASE/08_pipeline.br" --pipeline="$SHOWCASE/08_pipeline.txt" --steps=1 "$STORE"
printf 'dce\n' > "$TMP/store/runs/r3/pipeline.txt"
contains "resume: runs the edited pipeline" "[2] dce  ok" "$BIN" resume r3 "$STORE"
contains "a failing stage is rolled back" "unknown pass 'nope'" "$BIN" resume r1 --passes=nope "$STORE"
status "a failed run exits 2" 2 "$BIN" resume r1 --passes=nope "$STORE"
"$BIN" show r1@3 "$STORE" > "$TMP/edit.br" 2>/dev/null
sed -i 's/  return %y/  %z = relu %y\n  return %z/' "$TMP/edit.br"
contains "resume --state: a hand edit is a recorded stage" "edit.br  ok" \
  "$BIN" resume r1 --state="$TMP/edit.br" "$STORE"
contains "resume --state: ids are recovered by name" "matched" \
  "$BIN" show r1@6 --record "$STORE"
sed 's/%z = relu %y/%z = relu %nope/' "$TMP/edit.br" > "$TMP/broken.br"
contains "a broken edit is rejected" "unknown value '%nope'" \
  "$BIN" resume r1 --state="$TMP/broken.br" "$STORE"
contains "insert-guards discharges Unknowns" "status: done" \
  "$BIN" run "$CASES/warn_fresh_dims.br" --passes=insert-guards "$STORE"
last="$("$BIN" runs "$STORE" | tail -1 | cut -d' ' -f1)"
"$BIN" show "$last" "$STORE" > "$TMP/guarded.br" 2>/dev/null
contains "after insert-guards the file checks clean" "errors: 0, warnings: 0" "$BIN" check "$TMP/guarded.br"
contains "a guard names the fresh dimensions" "guard dim(%a, 0), dim(%b, 0) rel=eq" cat "$TMP/guarded.br"
contains "fold-reshape uses the facts" "status: done" \
  "$BIN" run "$EXAMPLES/reshape_facts.br" --passes=fold-reshape "$STORE"
last="$("$BIN" runs "$STORE" | tail -1 | cut -d' ' -f1)"
contains "fold-reshape removes the chain" "  return %x" "$BIN" show "$last" "$STORE"

if [ "$fail" -ne 0 ]; then
  echo "SOME TESTS FAILED"
  exit 1
fi
echo "all CLI tests passed"
