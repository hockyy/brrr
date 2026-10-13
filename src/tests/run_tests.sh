#!/usr/bin/env bash
# BridgeIR core tests: stable identity, editable names, canonical text.
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
   echo "$rename_out" | grep -q "n5 out = ret total"; then
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

exit $fail
