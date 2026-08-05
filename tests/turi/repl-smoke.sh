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

# --- Collection results render via their Show instance (repl-show-collections) ---
# The REPL preloads typeclass-show.tur so a bare Vec/Set/Map result prints its
# contents instead of an opaque pointer (turi_try_show_by_tag).
check "vec display"   "=> [1 2 3]"         "$(repl_out '(vec-of 1 2 3)')"
check "vec literal"   "=> [9 8 7]"         "$(repl_out '[9 8 7]')"
check "empty vec"     "=> []"              "$(repl_out '(vec-new)')"
check "set display"   "=> #set{7 8 9}"     "$(repl_out '(set-of 7 8 9)')"
check "map display"   "=> #map{1 10 2 20}" "$(repl_out '#map{1 10 2 20}')"
# (show 5) yields the cstr "5", which the prompt prints quoted.
check "show int"      '=> "5"'             "$(repl_out '(show 5)')"

# --- Carrier-list ops resolve to the runtime natives, not the unhandled
#     elaborator builtin (repl-list-head-over-cons-returns-nil).  Without the
#     native-function stubs the REPL preload injects, `cons` elaborates to a
#     BS_FUNC_CALL builtin the tree-walker cannot execute, so it (and any
#     list-head over it) returned nil at the prompt while --interpret and the
#     compiled path returned 65. ---
check "list-head over cons" "=> 65" "$(repl_out '(list-head (cons 65 (cons 66 0)))')"
check "head over cons"      "=> 42" "$(repl_out '(head (cons 42 0))')"

# --- Multi-line: split across two input lines ---
check "multi-line +"  "=> 3"  "$(repl_out '(+' '1 2)')"
check "multi-line let" "=> 7" "$(repl_out '(let [x 3' '      y 4] (+ x y))')"

# --- Persistent state ---
check "defn persists" "=> 25" "$(repl_out '(defn sq [x] (* x x))' '(sq 5)')"

# --- Reader macros persist across REPL turns (RM Q#5) ---
# A `(reader-macros/define ...)` on one line should affect subsequent reads;
# the registry is owned by TuriEnv, not the per-turn reader.
RM_OUT="$(repl_out "(reader-macros/define 'pi :none '3.14159)" '(println #pi)')"
check "reader macro persists" "3.14159" "$RM_OUT"

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

# --- :explain ---
# Case 1: Bare :explain with no recent diagnostic to explain
EXPLAIN_NONE_OUT="$(repl_out ":explain")"
check "bare :explain with no error" "no recent diagnostic to explain" "$EXPLAIN_NONE_OUT"

# Case 2: Specific code lookup (:explain TUR-E0001)
EXPLAIN_SPEC_OUT="$(repl_out ":explain TUR-E0001")"
check ":explain TUR-E0001" "Type mismatch" "$EXPLAIN_SPEC_OUT"

# Case 3: Specific code lookup normalized to uppercase (:explain tur-e0001)
EXPLAIN_NORM_OUT="$(repl_out ":explain tur-e0001")"
check ":explain tur-e0001" "Type mismatch" "$EXPLAIN_NORM_OUT"

# Case 4: Specific code miss (:explain TUR-E9999)
EXPLAIN_MISS_OUT="$(repl_out ":explain TUR-E9999")"
check ":explain TUR-E9999" "unknown diagnostic code 'TUR-E9999'" "$EXPLAIN_MISS_OUT"

# Case 5: Bare :explain after a diagnostic is triggered
EXPLAIN_AFTER_OUT="$(repl_out "(defn f [] invalid-name)" ":explain")"
check "bare :explain after unbound symbol" "Unbound symbol" "$EXPLAIN_AFTER_OUT"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
