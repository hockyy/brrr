#!/usr/bin/env bash
# BridgeIR core tests: stable identity + canonical text.
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

exit $fail
