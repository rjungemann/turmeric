#!/usr/bin/env bash
# tests/turi/repl-multiline-input.sh -- the REPL reads a multi-line form the
# way the reader does (repl-continuation-counter-misreads-reader-syntax).
#
# The continuation prompt used to be driven by a per-line bracket count that
# knew only `"` strings and `;` comments.  Every form below is valid in a file,
# and each one either kept the `..` prompt open until end of input (so piped
# input was swallowed with no output and exit 0) or was evaluated halfway
# through.  Because the failure is SILENCE, every case asserts on output that
# must appear -- the form's own `=> ...` line and a sentinel `(+ 2 3)` after it
# -- and on reader diagnostics that must not.
#
# The inline-C cases assert only that the `defn` is read as ONE form.  Whether
# the interpreter can then RUN the body is a separate question (it cannot run
# a loop body today; see aot-compiled-repl-plan C1), so the call's result is
# not part of this test.
set -euo pipefail

REPL="${1:-./build/tur}"
PASS=0
FAIL=0

# The interpreter keeps its closures for the process lifetime by design, so a
# sanitized (Debug) `tur repl` can exit non-zero on a LeakSanitizer report --
# `(display #\()` under #lang r7rs does -- and pipefail would then abort this
# script on a finding that is not what it tests.  Same default as
# repl-lang-r7rs.sh and the other turi harnesses (CLAUDE.md, leak policy).
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# Pipe stdin to the REPL; stdout+stderr, ANSI stripped (the same sed as
# repl-smoke.sh; no OSC 133 markers are written over a pipe --
# repl-host-integration.sh asserts that).  No auto-spice: the repo root is not
# a spice, but keep the session hermetic anyway.
repl_out() {
    TUR_NO_AUTO_SPICE=1 "$REPL" repl 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
}

has() {
    local desc="$1" needle="$2" actual="$3"
    if grep -qF -- "$needle" <<< "$actual"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected to contain: $needle"
        echo "  got:"
        sed 's/^/    /' <<< "$actual"
        FAIL=$((FAIL + 1))
    fi
}

lacks() {
    local desc="$1" needle="$2" actual="$3"
    if grep -qF -- "$needle" <<< "$actual"; then
        echo "FAIL: $desc"
        echo "  expected NOT to contain: $needle"
        echo "  got:"
        sed 's/^/    /' <<< "$actual"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    fi
}

# --- A C `for` header: its `(` and `;` are C, not Lisp (was: prompt never
#     closed, everything after swallowed, exit 0) ---
out="$(printf '%s\n' \
    '(defn c-mix [a : int b : int] : int' \
    '  ```c' \
    '  int64_t t = a;' \
    '  for (int i = 0; i < 3; i++) t = t * 31 + b;' \
    '  return t;' \
    '  ```)' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has   "for-loop body: defn read as one form"   "=> #<fn c-mix>" "$out"
has   "for-loop body: next form evaluated"     "=> 5"           "$out"
lacks "for-loop body: nothing cancelled"       "(cancelled)"    "$out"

# --- A C char literal ')': one close paren with no open (was: evaluated
#     halfway through the fence, four bogus diagnostics) ---
out="$(printf '%s\n' \
    '(defn is-close [c : int] : bool' \
    '  ```c' \
    "  if (c == ')') return 1;" \
    '  return 0;' \
    '  ```)' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has   "char-literal paren: defn read as one form"  "=> #<fn is-close>"         "$out"
has   "char-literal paren: next form evaluated"    "=> 5"                      "$out"
lacks "char-literal paren: no split fence"         "unterminated C code block" "$out"
lacks "char-literal paren: no body read as Lisp"   "unbound symbol 'return'"   "$out"

# --- A C brace block over several lines, and a C `//` comment holding a
#     paren: brackets inside the fence are not counted at all ---
out="$(printf '%s\n' \
    '(defn clampish [x : int] : int' \
    '  ```c' \
    '  if (x > 10) {  // clamp (high' \
    '    return 10;' \
    '  }' \
    '  return x;' \
    '  ```)' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has "brace block: defn read as one form" "=> #<fn clampish>" "$out"
has "brace block: next form evaluated"   "=> 5"              "$out"

# --- A blank line between C statements is content, not a cancel (was:
#     `(cancelled)`, then the rest of the body read as Lisp) ---
out="$(printf '%s\n' \
    '(defn two [] : int' \
    '  ```c' \
    '  int64_t t = 1;' \
    '' \
    '  return t + 1;' \
    '  ```)' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has   "blank line in fence: defn read as one form" "=> #<fn two>" "$out"
has   "blank line in fence: next form evaluated"   "=> 5"         "$out"
lacks "blank line in fence: not cancelled"         "(cancelled)"  "$out"

# --- A string spanning lines, with a paren on its second line ---
out="$(printf '%s\n' \
    '(println "a' \
    '(b")' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has "multi-line string: printed" "(b"   "$out"
has "multi-line string: next"    "=> 5" "$out"

# --- A blank line inside a string is part of the string ---
out="$(printf '%s\n' \
    '(println "x' \
    '' \
    'y")' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has   "blank line in string: printed tail" "y"           "$out"
has   "blank line in string: next"         "=> 5"        "$out"
lacks "blank line in string: not cancelled" "(cancelled)" "$out"

# --- A block comment holding a paren, nested: the `(` sits after the INNER
#     `|#` but still inside the outer comment, so a scanner that stops at the
#     first `|#` (as well as the old per-line count) sees it as open ---
out="$(printf '%s\n' \
    '(+ 1 #| a #| b |# ( |# 2)' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has "block comment: evaluated" "=> 3" "$out"
has "block comment: next"      "=> 5" "$out"

# --- A character literal holding a paren (Turmeric reads #\c too) ---
out="$(printf '%s\n' \
    '#lang r7rs' \
    '(display #\()' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has "r7rs char literal: next form evaluated" "=> 5" "$out"

# --- Still a continuation: an open bracket keeps reading, and a blank line
#     outside any string/fence still cancels ---
out="$(printf '%s\n' \
    '(+ 1' \
    '   2)' \
    ':quit' | repl_out)"
has "open bracket continues" "=> 3" "$out"

out="$(printf '%s\n' \
    '(+ 1' \
    '' \
    '(+ 2 3)' \
    ':quit' | repl_out)"
has "blank line outside a lexeme cancels" "(cancelled)" "$out"
has "after cancel, next form evaluated"   "=> 5"        "$out"

# --- An unfinished form at end of input says so instead of vanishing ---
out="$(printf '%s\n' '(+ 1' | repl_out)"
has "EOF mid-form is reported" "(cancelled)" "$out"

echo ""
echo "repl-multiline-input: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
