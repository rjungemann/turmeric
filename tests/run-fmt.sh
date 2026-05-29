#!/usr/bin/env bash
# tests/run-fmt.sh -- integration tests for `tur fmt`
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests/run-fmt.sh: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

# ---------------------------------------------------------------------------
# Test: --stdin round-trips a well-formed s-expression
# ---------------------------------------------------------------------------
NAME="fmt-stdin-identity"
INPUT='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$INPUT" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$INPUT', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin normalises extra whitespace
# ---------------------------------------------------------------------------
NAME="fmt-stdin-normalize"
INPUT='(defn add   [x :int y :int] :int (+ x y))'
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin exit code is 0 on success
# ---------------------------------------------------------------------------
NAME="fmt-stdin-exit-ok"
printf '(println 42)\n' | "$TUR" fmt --stdin > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --stdin rejects non-ASCII input (exit 1)
# ---------------------------------------------------------------------------
NAME="fmt-stdin-nonascii-exit"
printf '(def x \xe2\x80\x94 1)\n' | "$TUR" fmt --stdin > /dev/null 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected non-zero exit for non-ASCII input, got 0"
fi

# ---------------------------------------------------------------------------
# Test: --stdout formats a file and writes to stdout (file unchanged)
# ---------------------------------------------------------------------------
NAME="fmt-stdout-file"
TMPDIR_FMT=$(mktemp -d)
printf '(defn foo   [x :int] :int x)\n' > "$TMPDIR_FMT/test.tur"
ORIG=$(cat "$TMPDIR_FMT/test.tur")
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_FMT/test.tur" 2>/dev/null)
AFTER=$(cat "$TMPDIR_FMT/test.tur")
if [ "$ACTUAL" = "(defn foo [x :int] :int x)" ] && [ "$ORIG" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "stdout='$ACTUAL', file_unchanged=$([ "$ORIG" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 0 when file is already formatted
# ---------------------------------------------------------------------------
NAME="fmt-check-already-formatted"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/clean.tur"
"$TUR" fmt --check "$TMPDIR_FMT/clean.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for already-formatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 1 when file needs formatting
# ---------------------------------------------------------------------------
NAME="fmt-check-needs-format"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 1 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 1 for unformatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check does not modify the file
# ---------------------------------------------------------------------------
NAME="fmt-check-no-modify"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
BEFORE=$(cat "$TMPDIR_FMT/dirty.tur")
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
if [ "$BEFORE" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "--check modified the file"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --dry-run is an alias for --check
# ---------------------------------------------------------------------------
NAME="fmt-dry-run-alias"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --dry-run "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
BEFORE='(defn add   [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 1 ] && [ "$AFTER" = "$BEFORE" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, file_unchanged=$([ "$BEFORE" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: in-place format modifies file and exits 0
# ---------------------------------------------------------------------------
NAME="fmt-inplace"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 0 ] && [ "$AFTER" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, content='$AFTER'"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: directory walk skips build/ and .git/
# ---------------------------------------------------------------------------
NAME="fmt-walk-skip-dirs"
TMPDIR_FMT=$(mktemp -d)
mkdir -p "$TMPDIR_FMT/src" "$TMPDIR_FMT/build" "$TMPDIR_FMT/.git"
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/src/ok.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/build/skip.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/.git/skip.tur"
BUILD_BEFORE=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_BEFORE=$(cat "$TMPDIR_FMT/.git/skip.tur")
"$TUR" fmt "$TMPDIR_FMT" > /dev/null 2>&1
BUILD_AFTER=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_AFTER=$(cat "$TMPDIR_FMT/.git/skip.tur")
if [ "$BUILD_BEFORE" = "$BUILD_AFTER" ] && [ "$GIT_BEFORE" = "$GIT_AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "formatter touched files in skipped directories"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: unknown flag exits 2
# ---------------------------------------------------------------------------
NAME="fmt-unknown-flag"
printf '(println 1)\n' | "$TUR" fmt --unknown-flag > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 2 for unknown flag, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --lang tursweet accepted with --stdin
# ---------------------------------------------------------------------------
NAME="fmt-lang-tursweet-stdin"
printf 'defn add [x :int y :int] :int\n  +(x y)\n' | "$TUR" fmt --stdin --lang tursweet > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --lang tursweet, got $RC"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "fmt summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
