#!/usr/bin/env bash
# tests/run-parse-check.sh -- integration tests for `tur parse-check`
#
# `tur parse-check <a> <b>` reads two files and compares the form trees they
# produce. Arg 1 is read as turmeric, arg 2 as sweet-exp, unless an explicit
# `#lang` directive overrides. Exit codes: 0 equal, 1 mismatch, 2 read error.
# This backs tools/check-guide-pairs.py.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests/run-parse-check.sh: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

TMPDIR_PC=$(mktemp -d)
trap 'rm -rf "$TMPDIR_PC"' EXIT

# ---------------------------------------------------------------------------
# Test: equal pair (sweet side carries a #lang directive) exits 0
# ---------------------------------------------------------------------------
NAME="parse-check-equal-with-directive"
printf '(defn square [x :float] :float\n  (* x x))\n' > "$TMPDIR_PC/a.tur"
printf '#lang sweet-exp\ndefn square [x :float] :float\n  *(x x)\n' > "$TMPDIR_PC/a.sweet"
"$TUR" parse-check "$TMPDIR_PC/a.tur" "$TMPDIR_PC/a.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then pass "$NAME"; else fail "$NAME" "expected exit 0, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: equal inline pair (no #lang directive; arg 2 forced sweet) exits 0
# ---------------------------------------------------------------------------
NAME="parse-check-equal-inline"
printf '(println (str-concat "Hello, " name))\n' > "$TMPDIR_PC/b.tur"
printf 'println $ str-concat "Hello, " name\n' > "$TMPDIR_PC/b.sweet"
"$TUR" parse-check "$TMPDIR_PC/b.tur" "$TMPDIR_PC/b.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then pass "$NAME"; else fail "$NAME" "expected exit 0, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: neoteric inline pair exits 0
# ---------------------------------------------------------------------------
NAME="parse-check-equal-neoteric"
printf '(println (str-concat "Hello, " name))\n' > "$TMPDIR_PC/c.tur"
printf 'println(str-concat("Hello, " name))\n' > "$TMPDIR_PC/c.sweet"
"$TUR" parse-check "$TMPDIR_PC/c.tur" "$TMPDIR_PC/c.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then pass "$NAME"; else fail "$NAME" "expected exit 0, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: genuinely different ASTs exit 1
# ---------------------------------------------------------------------------
NAME="parse-check-mismatch"
printf '(defn cube [x :float] :float\n  (* x (* x x)))\n' > "$TMPDIR_PC/d.sweet"
"$TUR" parse-check "$TMPDIR_PC/a.tur" "$TMPDIR_PC/d.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 1 ]; then pass "$NAME"; else fail "$NAME" "expected exit 1, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: malformed input exits 2
# ---------------------------------------------------------------------------
NAME="parse-check-read-error"
printf '(defn broken [x\n' > "$TMPDIR_PC/e.tur"
"$TUR" parse-check "$TMPDIR_PC/e.tur" "$TMPDIR_PC/a.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then pass "$NAME"; else fail "$NAME" "expected exit 2, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: missing file exits 2
# ---------------------------------------------------------------------------
NAME="parse-check-missing-file"
"$TUR" parse-check "$TMPDIR_PC/does-not-exist.tur" "$TMPDIR_PC/a.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then pass "$NAME"; else fail "$NAME" "expected exit 2, got $RC"; fi

# ---------------------------------------------------------------------------
# Test: --help prints usage and exits 0
# ---------------------------------------------------------------------------
NAME="parse-check-help"
"$TUR" parse-check --help > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then pass "$NAME"; else fail "$NAME" "expected exit 0 for --help, got $RC"; fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "parse-check summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
