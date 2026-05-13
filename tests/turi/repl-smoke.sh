#!/usr/bin/env bash
# tests/turi/repl-smoke.sh — Phase S1 REPL smoke tests
# Verifies multi-line input, meta-commands (:help, :type, :doc), and
# persistent definitions across expressions.
set -euo pipefail

REPL="${1:-./build/tur}"
PASS=0
FAIL=0

# Pipe lines to the REPL and capture stdout (ANSI codes stripped for robustness)
repl_out() {
    printf '%s\n' "$@" | "$REPL" repl 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'
}

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected substring: $expected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

# --- Basic evaluation ---
check "(+ 1 2)"       "=> 3"       "$(repl_out '(+ 1 2)')"
check "(* 6 7)"       "=> 42"      "$(repl_out '(* 6 7)')"
check "string literal" '"hello"'   "$(repl_out '"hello"')"

# --- Multi-line: split across two input lines ---
check "multi-line +"  "=> 3"  "$(repl_out '(+' '1 2)')"
check "multi-line let" "=> 7" "$(repl_out '(let [x 3' '      y 4] (+ x y))')"

# --- Persistent state ---
check "defn persists" "=> 25" "$(repl_out '(defn sq [x] (* x x))' '(sq 5)')"

# --- :type meta-command ---
TYPE_OUT="$(repl_out ':type (+ 1 2)')"
check ":type int"     ": int"    "$TYPE_OUT"

TYPE_BOOL="$(repl_out ':type (< 1 2)')"
check ":type bool"    ": bool"   "$TYPE_BOOL"

# --- :doc meta-command ---
DOC_OUT="$(repl_out ':doc +')"
check ":doc +"        "add"      "$DOC_OUT"

DOC_DEFN="$(repl_out '(defn greet [] "hi")' ':doc greet')"
check ":doc user def" "greet"    "$DOC_DEFN"

# --- :help meta-command ---
HELP_OUT="$(repl_out ':help')"
check ":help shows :quit"    ":quit"   "$HELP_OUT"
check ":help shows :type"    ":type"   "$HELP_OUT"
check ":help shows :doc"     ":doc"    "$HELP_OUT"
check ":help shows :reload"  ":reload" "$HELP_OUT"

# --- :quit exits cleanly ---
EXIT_CODE=0
printf ':quit\n' | "$REPL" repl >/dev/null 2>&1 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ]; then
    echo "PASS: :quit exits 0"
    PASS=$((PASS + 1))
else
    echo "FAIL: :quit exited with code $EXIT_CODE"
    FAIL=$((FAIL + 1))
fi

# --- :reload <file> ---
TMP=$(mktemp /tmp/repl_smoke_XXXXXX.tur)
echo '(defn answer [] 42)' > "$TMP"
RELOAD_OUT="$(repl_out ":reload $TMP" '(answer)')"
rm -f "$TMP"
check ":reload file" "=> 42" "$RELOAD_OUT"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
