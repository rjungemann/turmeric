#!/usr/bin/env bash
# tests/turi/repl-lang-r7rs.sh -- r7rs-lang-plan R9: the REPL as an R7RS
# session, by `--lang r7rs` and by `#lang r7rs` typed at the prompt.
#
# The probe must tell the three layers of a Scheme session apart, because
# each one was missing at some point:
#
#   - the LANGUAGE (unannotated parameters are `any`): a float add through an
#     unannotated `define`, 3.75 -- `(add 1 2)` would be 3 under any dialect;
#   - the PRELUDE and its RENAMES: `(write (list 'a "b" #\c))` must print
#     `(a "b" #\c)`.  `#lang r7rs` at the prompt used to switch the language
#     and keep Turmeric's preload, and the prompt's `<eval>` text was exempt
#     from the renames, so `write` was an unknown name;
#   - the ECHO: `(car '(x y))` echoes `=> x` through the prelude's `write`,
#     where the Turmeric echo said `#<struct Sym>`.
#
# Plus: `--lang r7rs` was refused outright ("expected turmeric or saffron");
# switching back with `#lang turmeric` must restore Turmeric's int default;
# `:reset` keeps the session's dialect.
#
# Skips cleanly (exit 0) when the binary is missing, like its siblings.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built"
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi
# The interpreter's closures are process-lifetime by design (CLAUDE.md).
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

fails=0
run_repl() {   # run_repl <input> [flags...]
    local input="$1"; shift
    printf '%s' "$input" | "$TUR" repl "$@" 2>&1
}

PROBE='(define (add a b) (+ a b))
(add 1.5 2.25)
(write (list (quote a) "b" #\c))
(newline)
(car (quote (x y)))
'

check_scheme() {   # check_scheme <label> <output>
    local label="$1" out="$2"
    if grep -q '=> 3\.75' <<<"$out" && grep -q '(a "b" #\\c)' <<<"$out" \
       && grep -qx '=> x' <<<"$out"; then
        echo "ok   $label: any params, prelude renames, Scheme echo"
    else
        echo "FAIL $label: expected 3.75, (a \"b\" #\\c) and => x"
        sed 's/^/       /' <<<"$out" | tail -10
        fails=$((fails + 1))
    fi
}

# --- 1. --lang r7rs --------------------------------------------------------
check_scheme "--lang r7rs" "$(run_repl "$PROBE" --lang r7rs)"

# --- 2. #lang r7rs at the prompt -------------------------------------------
out="$(run_repl "#lang r7rs
$PROBE")"
check_scheme "#lang r7rs at the prompt" "$out"
if ! grep -q 'language set to r7rs' <<<"$out"; then
    echo "FAIL #lang r7rs at the prompt: the switch is not reported"
    fails=$((fails + 1))
fi

# --- 3. back to Turmeric -----------------------------------------------------
out="$(run_repl "#lang r7rs
$PROBE#lang turmeric
(defn add [a b] (+ a b))
(add 1.5 2.25)
")"
if grep -q 'expected int, got float' <<<"$out"; then
    echo "ok   #lang turmeric after r7rs: Turmeric's int default is back"
else
    echo "FAIL #lang turmeric after r7rs: expected the int-default error"
    sed 's/^/       /' <<<"$out" | tail -8
    fails=$((fails + 1))
fi

# --- 4. :reset keeps the dialect ----------------------------------------------
out="$(run_repl ":reset
$PROBE" --lang r7rs)"
check_scheme ":reset in an r7rs session" "$out"

if [ "$fails" -ne 0 ]; then
    echo "repl-lang-r7rs: $fails check(s) failed"
    exit 1
fi
echo "repl-lang-r7rs: all checks passed"
