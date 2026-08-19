#!/usr/bin/env bash
# tests/run-flags.sh — Tests for global flags that can't be expressed as
# standard fixture tests (--explain, --dump-kinds, etc.)
#
# Each test case is a short bash block that prints "PASS <name>" on success
# or "FAIL <name> — <reason>" on failure, then increments the counter.
#
# Exit status: 0 if all tests pass, 1 if any fail.
#
# Phase HKT-P5: tur-explain-kind-mismatch
# Phase HKT-P6: dump-kinds-basic

set -uo pipefail
cd "$(dirname "$0")/.."

# This harness exercises the tree-walking turi/eval interpreter (the eval-*
# cases below), which intentionally never frees its closures or registered
# natives -- they live for the process lifetime. LeakSanitizer (enabled with
# ASan on Linux) would otherwise flag that design choice as a leak. Default to
# detect_leaks=0 to mirror the ctest policy (tur_flags_tests) so a direct
# `bash tests/run-flags.sh` does not surprise contributors; opt back in with
# ASAN_OPTIONS=detect_leaks=1 bash tests/run-flags.sh. See
# docs/asan-debug-leaks-plan.md.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# Overridable so the jit-ffi cases below can be exercised against a
# -DTUR_JIT=ON build (`TUR=./build-jit/tur bash tests/run-flags.sh`),
# matching run-jit.sh's convention.  Default unchanged.
TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 — $2"; }

# ---------------------------------------------------------------------------
# HKT-P5 tests
# ---------------------------------------------------------------------------

# tur-explain-kind-mismatch: --explain TUR-E0012 should produce non-empty
# output containing "Kind" and exit 0.
out=$("$TUR" --explain TUR-E0012 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-kind-mismatch" "non-zero exit ($rc)"
elif [ -z "$out" ]; then
    fail "tur-explain-kind-mismatch" "empty output"
elif ! echo "$out" | grep -qi "Kind"; then
    fail "tur-explain-kind-mismatch" "output did not mention 'Kind'"
else
    pass "tur-explain-kind-mismatch"
fi

# tur-explain-orphan-instance: --explain TUR-E0013 should mention "orphan"
out=$("$TUR" --explain TUR-E0013 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-orphan-instance" "non-zero exit ($rc)"
elif ! echo "$out" | grep -qi "orphan\|instance"; then
    fail "tur-explain-orphan-instance" "output did not mention 'orphan/instance'"
else
    pass "tur-explain-orphan-instance"
fi

# tur-explain-all-codes: every known TUR-E code should have an explanation
for code in TUR-E0001 TUR-E0002 TUR-E0003 TUR-E0004 TUR-E0005 \
            TUR-E0007 TUR-E0009 TUR-E0010 TUR-E0011 TUR-E0012 TUR-E0013 TUR-E0021; do
    out=$("$TUR" --explain "$code" 2>&1); rc=$?
    if [ $rc -ne 0 ] || [ -z "$out" ]; then
        fail "tur-explain-${code}" "no explanation registered (exit=$rc)"
    else
        pass "tur-explain-${code}"
    fi
done

# tur-explain-unknown-code: --explain TUR-E9999 should exit non-zero
out=$("$TUR" --explain TUR-E9999 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "tur-explain-unknown-code" "expected non-zero exit, got 0"
else
    pass "tur-explain-unknown-code"
fi

# ---------------------------------------------------------------------------
# HKT-P6 tests
# ---------------------------------------------------------------------------

# dump-kinds-basic: --dump-kinds emit-c on a file with a KIND_ARROW typeclass
# should print "defclass Functor param[0] : * -> *" to stdout.
FIXTURE="tests/fixtures/dump-kinds-basic/input.tur"
out=$("$TUR" --dump-kinds emit-c "$FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-kinds-basic" "non-zero exit ($rc)"
elif [[ "$out" != *"defclass Functor param[0] : * -> *"* ]]; then
    fail "dump-kinds-basic" "expected 'defclass Functor param[0] : * -> *' in output"
else
    pass "dump-kinds-basic"
fi

# dump-kinds-no-output: without --dump-kinds, the same file should NOT print
# kind annotations mixed into the C output.
out=$("$TUR" emit-c "$FIXTURE" 2>/dev/null); rc=$?
if echo "$out" | grep -q "defclass Functor param"; then
    fail "dump-kinds-no-output" "kind dump appeared without --dump-kinds flag"
else
    pass "dump-kinds-no-output"
fi

# ---------------------------------------------------------------------------
# Phase M3: separate compilation (tur build <dir>)
# ---------------------------------------------------------------------------

# m3-separate-compilation: 'tur build <dir>' with two modules — one library
# exporting `add`, one entry point that calls it — should produce a working
# binary.  We verify by checking the exit code is the expected value.
_m3_dir="$(mktemp -d -t tur-m3-XXXXXX)"
cat > "$_m3_dir/adder.tur" <<'EOF_ADDER'
(defmodule adder
  (export add)
  (defn add [x :int y :int] :int (+ x y)))
EOF_ADDER
cat > "$_m3_dir/myapp.tur" <<'EOF_MYAPP'
(defmodule myapp
  (import adder :as a)
  (defn main [] :int
    (- (a/add 3 4) 7)))
EOF_MYAPP
_m3_exe="$(mktemp -t tur-m3-XXXXXX)"
"$TUR" build "$_m3_dir" -o "$_m3_exe" 2>/tmp/tur-m3-build.err
_m3_build_rc=$?
if [ "$_m3_build_rc" -ne 0 ]; then
    fail "m3-separate-compilation" "build failed (rc=$_m3_build_rc): $(cat /tmp/tur-m3-build.err)"
else
    "$_m3_exe"; _m3_run_rc=$?
    if [ "$_m3_run_rc" -ne 0 ]; then
        fail "m3-separate-compilation" "binary returned unexpected exit code $_m3_run_rc (expected 0)"
    else
        pass "m3-separate-compilation"
    fi
fi
rm -f "$_m3_exe" /tmp/tur-m3-build.err
rm -rf "$_m3_dir"

# m3-cross-module-symbol-mangling: verify that exported symbols get the
# module-prefix mangling in the generated header (adder__add, not just add).
_m3_dir2="$(mktemp -d -t tur-m3b-XXXXXX)"
cat > "$_m3_dir2/adder.tur" <<'EOF_ADDER2'
(defmodule adder
  (export add)
  (defn add [x :int y :int] :int (+ x y)))
EOF_ADDER2
_m3_hbuf="$(mktemp -t tur-m3h-XXXXXX)"
"$TUR" emit-h "$_m3_dir2/adder.tur" > "$_m3_hbuf" 2>/dev/null
if grep -q "adder__add" "$_m3_hbuf"; then
    pass "m3-cross-module-symbol-mangling"
else
    fail "m3-cross-module-symbol-mangling" "expected 'adder__add' in header; got: $(cat "$_m3_hbuf")"
fi
rm -f "$_m3_hbuf"
rm -rf "$_m3_dir2"

# ---------------------------------------------------------------------------
# Formatter round-trip tests (defpackage / deflockfile / comment preservation)
# ---------------------------------------------------------------------------

# fmt-defpackage-basic: a defpackage wide enough to exceed line width should
# format with each :key val pair on its own line.
_fmt_input='(defpackage my-project-with-longer-name :name "my-project-with-longer-name" :version "0.1.0" :description "A test package")'
_fmt_out=$(echo "$_fmt_input" | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-basic" "tur format exited $rc: $_fmt_out"
elif ! echo "$_fmt_out" | grep -q ':name'; then
    fail "fmt-defpackage-basic" ":name not present in output: $_fmt_out"
elif ! echo "$_fmt_out" | grep -q '^  :version'; then
    fail "fmt-defpackage-basic" ":version not on its own indented line: $_fmt_out"
else
    pass "fmt-defpackage-basic"
fi

# fmt-defpackage-round-trip: formatting a defpackage twice should be idempotent.
_fmt1=$(printf '(defpackage my-project-with-longer-name\n  :name    "my-project-with-longer-name"\n  :version "0.1.0")' | "$TUR" format 2>&1)
_fmt2=$(echo "$_fmt1" | "$TUR" format 2>&1)
if [ "$_fmt1" != "$_fmt2" ]; then
    fail "fmt-defpackage-round-trip" "formatter not idempotent; first pass != second pass"
else
    pass "fmt-defpackage-round-trip"
fi

# fmt-defpackage-spices-block: a defpackage with multiple spices should expand
# the :spices map to block layout when it doesn't fit on one line.
_fmt_spices_in='(defpackage my-application :name "my-application" :version "0.1.0" :spices #{"geom" #{ :url "https://github.com/alice/tur-geom" :ref "v0.2.1"} "http" #{ :url "https://github.com/alice/tur-http" :ref "v1.0.0"}})'
_fmt_spices_out=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-spices-block" "tur format exited $rc"
elif ! echo "$_fmt_spices_out" | grep -q '#{$'; then
    fail "fmt-defpackage-spices-block" ":spices map was not expanded to block: $_fmt_spices_out"
else
    pass "fmt-defpackage-spices-block"
fi

# fmt-defpackage-spices-idempotent: the spices block output is itself idempotent.
_fmt_spices1=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1)
_fmt_spices2=$(echo "$_fmt_spices1" | "$TUR" format 2>&1)
if [ "$_fmt_spices1" != "$_fmt_spices2" ]; then
    fail "fmt-defpackage-spices-idempotent" "formatter not idempotent for spices block"
else
    pass "fmt-defpackage-spices-idempotent"
fi

# fmt-defpackage-comments: a defpackage with a leading comment should preserve
# the comment through a format round-trip.
_fmt_cmt_out=$(printf '; This is a comment\n(defpackage my-app\n  :name    "my-app"\n  :version "0.1.0")' | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-comments" "tur format exited $rc: $_fmt_cmt_out"
elif ! echo "$_fmt_cmt_out" | grep -q 'This is a comment'; then
    fail "fmt-defpackage-comments" "comment lost after formatting: $_fmt_cmt_out"
else
    pass "fmt-defpackage-comments"
fi

# fmt-deflockfile-round-trip: deflockfile form should be idempotent.
_fmt_lock1=$(printf '(deflockfile\n  :format-version 1\n  :spices         #{}\n  :cmake-deps     #{})' | "$TUR" format 2>&1)
_fmt_lock2=$(echo "$_fmt_lock1" | "$TUR" format 2>&1)
if [ "$_fmt_lock1" != "$_fmt_lock2" ]; then
    fail "fmt-deflockfile-round-trip" "formatter not idempotent for deflockfile"
else
    pass "fmt-deflockfile-round-trip"
fi

# ---------------------------------------------------------------------------
# ER1 tests: --strict-effects and TUR-W0030/W0031
# ---------------------------------------------------------------------------

# strict-effects-warn: an unannotated function that performs Write should trigger TUR-W0030
STRICT_INPUT=$(mktemp /tmp/tur-strict-XXXXXX.tur)
cat > "$STRICT_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" --strict-effects emit-c "$STRICT_INPUT" 2>&1) || true
rm -f "$STRICT_INPUT"
if echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    pass "strict-effects-warn"
else
    fail "strict-effects-warn" "expected TUR-W0030 in stderr output"
fi

# over-annotated-warn: a function that declares #{Write} but never performs it should trigger TUR-W0031
OVER_INPUT=$(mktemp /tmp/tur-over-XXXXXX.tur)
cat > "$OVER_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn pure-fn [] #{Write} :int
  42)
(defn main [] :int 0)
EOF
out=$("$TUR" emit-c "$OVER_INPUT" 2>&1) || true
rm -f "$OVER_INPUT"
if echo "$out" | grep -F "TUR-W0031" > /dev/null 2>&1; then
    pass "over-annotated-warn"
else
    fail "over-annotated-warn" "expected TUR-W0031 in stderr output"
fi

# strict-effects-clean: a fully annotated function should not warn under --strict-effects
CLEAN_INPUT=$(mktemp /tmp/tur-clean-XXXXXX.tur)
cat > "$CLEAN_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] #{Write} :nil
  (perform (Write "hello")))
(defn main [] :int
  (handle
    (do (effectful) 0)
    (Write [msg] k) (do (println msg) (resume k 0))))
EOF
out=$("$TUR" --strict-effects emit-c "$CLEAN_INPUT" 2>&1); rc=$?
rm -f "$CLEAN_INPUT"
if [ $rc -ne 0 ]; then
    fail "strict-effects-clean" "should succeed under --strict-effects when fully annotated (exit=$rc)"
elif echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    fail "strict-effects-clean" "should not warn TUR-W0030 when function has annotation"
else
    pass "strict-effects-clean"
fi

# ---------------------------------------------------------------------------
# ER6 tests: --dump-effects and --lint-effects
# ---------------------------------------------------------------------------

# dump-effects-basic: --dump-effects should print "defn effectful : #{Write}"
# Note: use a temp file to avoid grep -q SIGPIPE issue with large output + pipefail.
DUMP_FIXTURE="tests/fixtures/effect-dump/input.tur"
_dump_tmp=$(mktemp /tmp/tur-dump-XXXXXX)
"$TUR" --dump-effects emit-c "$DUMP_FIXTURE" 2>/dev/null > "$_dump_tmp"; rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-effects-basic" "non-zero exit ($rc)"
elif ! grep -q "defn effectful : #" "$_dump_tmp"; then
    fail "dump-effects-basic" "expected 'defn effectful : #...' in output; got: $(grep '^defn' "$_dump_tmp" | head -3)"
elif ! grep -q "defn pure-fn : #{}" "$_dump_tmp"; then
    fail "dump-effects-basic" "expected 'defn pure-fn : #{}' in output"
else
    pass "dump-effects-basic"
fi
rm -f "$_dump_tmp"

# dump-effects-no-output: without --dump-effects, the same file should not emit "defn" effect lines
_nodump_tmp=$(mktemp /tmp/tur-nodump-XXXXXX)
"$TUR" emit-c "$DUMP_FIXTURE" 2>/dev/null > "$_nodump_tmp"
if grep -q "^defn effectful : #" "$_nodump_tmp"; then
    fail "dump-effects-no-output" "effect dump appeared without --dump-effects flag"
else
    pass "dump-effects-no-output"
fi
rm -f "$_nodump_tmp"

# lint-effects-warn: --lint-effects on an unannotated effectful function should emit TUR-W0030
LINT_INPUT=$(mktemp /tmp/tur-lint-XXXXXX.tur)
cat > "$LINT_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" --lint-effects emit-c "$LINT_INPUT" 2>&1) || true
rm -f "$LINT_INPUT"
if echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    pass "lint-effects-warn"
else
    fail "lint-effects-warn" "expected TUR-W0030 in output under --lint-effects"
fi

# lint-effects-annotated: --lint-effects should not warn on an annotated effectful function
LINT_ANN_INPUT=$(mktemp /tmp/tur-lint-ann-XXXXXX.tur)
cat > "$LINT_ANN_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] #{Write} :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" --lint-effects emit-c "$LINT_ANN_INPUT" 2>&1) || true
rm -f "$LINT_ANN_INPUT"
if echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    fail "lint-effects-annotated" "should not warn TUR-W0030 for annotated function under --lint-effects"
else
    pass "lint-effects-annotated"
fi

# try-with-basic: try-with behaves identically to handle
out=$("$TUR" --interpret tests/fixtures/try-with-basic/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-basic" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'asking\n42')" ]; then
    fail "try-with-basic" "unexpected output: '$out'"
else
    pass "try-with-basic"
fi

# try-with-nested: nested try-with handlers work correctly
out=$("$TUR" --interpret tests/fixtures/try-with-nested/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-nested" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'start\noops')" ]; then
    fail "try-with-nested" "unexpected output: '$out'"
else
    pass "try-with-nested"
fi

# ---------------------------------------------------------------------------
# ER6: --check mode for effect-row errors
# ---------------------------------------------------------------------------

# check-mode-effect-error: `tur check` reports TUR-E0009 and exits 1 when a
# function's declared row does not include an effect it performs.
CHECK_INPUT=$(mktemp /tmp/tur-check-effect-XXXXXX.tur)
cat > "$CHECK_INPUT" << 'EOF'
(defeffect Write [s :cstr] :nil)
(defn bad [] #{} :nil
  (perform (Write "oops")))
(defn main [] :int 0)
EOF
out=$("$TUR" check "$CHECK_INPUT" 2>&1); rc=$?
rm -f "$CHECK_INPUT"
if [ $rc -eq 0 ]; then
    fail "check-mode-effect-error" "expected non-zero exit from tur check"
elif ! echo "$out" | grep -F "TUR-E0009" > /dev/null 2>&1; then
    fail "check-mode-effect-error" "expected TUR-E0009 in check output"
else
    pass "check-mode-effect-error"
fi

# check-mode-effect-ok: `tur check` exits 0 for a well-annotated program.
CHECK_OK_INPUT=$(mktemp /tmp/tur-check-ok-XXXXXX.tur)
cat > "$CHECK_OK_INPUT" << 'EOF'
(defeffect Write [s :cstr] :nil)
(defn do-write [] #{Write} :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" check "$CHECK_OK_INPUT" 2>&1); rc=$?
rm -f "$CHECK_OK_INPUT"
if [ $rc -ne 0 ]; then
    fail "check-mode-effect-ok" "expected zero exit from tur check (exit=$rc): $out"
else
    pass "check-mode-effect-ok"
fi

# ---------------------------------------------------------------------------
# PR5-3-B: effect-export-syntax
# ---------------------------------------------------------------------------

# effect-export-syntax: (export (effect Write)) is accepted by the parser/elab.
# stderr is dropped (the interpreter's sanitizer build emits a benign ASan
# makecontext/swapcontext warning there); rc captures elaboration errors and
# stdout carries the program output.
err=$(mktemp)
out=$("$TUR" --interpret tests/fixtures/effect-export-explicit/input.tur 2>"$err"); rc=$?
if [ $rc -ne 0 ]; then
    fail "effect-export-syntax" "should elaborate (exit=$rc): $(cat "$err")"
elif [ "$out" != "hello" ]; then
    fail "effect-export-syntax" "unexpected output: '$out'"
else
    pass "effect-export-syntax"
fi
rm -f "$err"

# ---------------------------------------------------------------------------
# E1: --help / -h (Tier 1)
# ---------------------------------------------------------------------------

# tur --help: should print usage and exit 0
out=$("$TUR" --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-global" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "usage:"; then
    fail "help-global" "output did not contain 'usage:'"
else
    pass "help-global"
fi

# tur -h: same as --help
out=$("$TUR" -h 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-short" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "usage:"; then
    fail "help-short" "output did not contain 'usage:'"
else
    pass "help-short"
fi

# tur build --help: should print subcommand help and exit 0
out=$("$TUR" build --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-build" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur build"; then
    fail "help-build" "output did not mention 'tur build'"
else
    pass "help-build"
fi

# tur run --help: should print subcommand help and exit 0
out=$("$TUR" run --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-run" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur run"; then
    fail "help-run" "output did not mention 'tur run'"
else
    pass "help-run"
fi

# tur check --help: should print subcommand help and exit 0
out=$("$TUR" check --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-check" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur check"; then
    fail "help-check" "output did not mention 'tur check'"
else
    pass "help-check"
fi

# tur eval --help: should print subcommand help and exit 0
out=$("$TUR" eval --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-eval" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur eval"; then
    fail "help-eval" "output did not mention 'tur eval'"
else
    pass "help-eval"
fi

# tur format --help: should print subcommand help and exit 0
out=$("$TUR" format --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-format" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur format"; then
    fail "help-format" "output did not mention 'tur format'"
else
    pass "help-format"
fi

# tur test --help: should print subcommand help and exit 0
out=$("$TUR" test --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-test" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur test"; then
    fail "help-test" "output did not mention 'tur test'"
else
    pass "help-test"
fi

# tur repl --help: should print subcommand help and exit 0
out=$("$TUR" repl --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-repl" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur repl"; then
    fail "help-repl" "output did not mention 'tur repl'"
else
    pass "help-repl"
fi

# ---------------------------------------------------------------------------
# E2: --version / -V (Tier 1)
# ---------------------------------------------------------------------------

# tur --version: should print "tur: the Turmeric compiler (v<version>)" and exit 0
out=$("$TUR" --version 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-long" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "^tur: the Turmeric compiler "; then
    fail "version-long" "output '$out' did not start with 'tur: the Turmeric compiler '"
else
    pass "version-long"
fi

# tur -V: same as --version
out=$("$TUR" -V 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-short" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "^tur: the Turmeric compiler "; then
    fail "version-short" "output '$out' did not start with 'tur: the Turmeric compiler '"
else
    pass "version-short"
fi

# ---------------------------------------------------------------------------
# E3: tur eval (Tier 1)
# ---------------------------------------------------------------------------

# tur eval: evaluate an arithmetic expression
out=$("$TUR" eval "(+ 1 2)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-basic" "expected exit 0, got $rc; output: $out"
elif [ "$out" != "3" ]; then
    fail "eval-basic" "expected '3', got '$out'"
else
    pass "eval-basic"
fi

# tur eval: evaluate a comparison (builtin)
out=$("$TUR" eval "(< 1 2)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-bool" "expected exit 0, got $rc; output: $out"
elif [ "$out" != "true" ]; then
    fail "eval-bool" "expected 'true', got '$out'"
else
    pass "eval-bool"
fi

# tur eval: nil result prints nothing
out=$("$TUR" eval "(def x 42)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-nil-silent" "expected exit 0, got $rc; output: $out"
else
    pass "eval-nil-silent"
fi

# tur eval: parse error exits nonzero
out=$("$TUR" eval "(+ 1" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "eval-error" "expected nonzero exit for bad syntax, got 0"
else
    pass "eval-error"
fi

# tur eval --file: run a .tur file through the interpreter
TMP_TUR=$(mktemp /tmp/tur_eval_test_XXXXXX.tur)
echo '(def x 42) (+ x 1)' > "$TMP_TUR"
out=$("$TUR" eval --file "$TMP_TUR" 2>&1); rc=$?
rm -f "$TMP_TUR"
if [ $rc -ne 0 ]; then
    fail "eval-file" "expected exit 0, got $rc; output: $out"
else
    pass "eval-file"
fi

# Phase R2: catch-unwind composes with ok?/err? through the interpreter.
# Regression guard for the fix that (a) types ok?/err?/some?/none? as :bool in
# the --interpret/eval path and (b) returns catch-unwind results in the native
# Result-box layout so the predicates recover ok vs caught-panic.
TMP_CU=$(mktemp /tmp/tur_catch_unwind_XXXXXX.tur)
cat > "$TMP_CU" <<'CUEOF'
(defn boom [] :int (panic "boom"))
(defn main [] :int
  (let [r1 (catch-unwind (fn [] :int 42))]
    (if (ok? r1) (println "ok") (println "not-ok")))
  (let [r2 (catch-unwind (fn [] :int (boom)))]
    (if (err? r2) (println "caught") (println "not-caught")))
  0)
CUEOF
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_CU" 2>&1); rc=$?
rm -f "$TMP_CU"
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "ok" && printf '%s' "$out" | grep -q "caught"; then
    pass "eval-catch-unwind"
else
    fail "eval-catch-unwind" "expected ok+caught, got rc=$rc output: $out"
fi

# Phase R2 (OQ#2): result-must / option-must raise a *catchable* panic instead
# of calling _exit(1), so catch-unwind recovers them and an uncaught one fires
# defers + exits nonzero with the standard panic message.
TMP_MUST=$(mktemp /tmp/tur_must_XXXXXX.tur)
cat > "$TMP_MUST" <<'MUSTEOF'
(defn main [] :int
  (let [r (catch-unwind (fn [] :int (result-must (err 7))))]
    (if (err? r) (println "must-caught") (println "must-not-caught")))
  (let [v (result-must (ok 5))]
    (if (= v 5) (println "must-passthrough") (println "must-wrong")))
  0)
MUSTEOF
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_MUST" 2>&1); rc=$?
rm -f "$TMP_MUST"
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "must-caught" && printf '%s' "$out" | grep -q "must-passthrough"; then
    pass "eval-must-catchable"
else
    fail "eval-must-catchable" "expected must-caught+must-passthrough, got rc=$rc output: $out"
fi

# An uncaught result-must panic exits nonzero (does not silently _exit(0)).
TMP_MUST2=$(mktemp /tmp/tur_must2_XXXXXX.tur)
echo '(defn main [] :int (result-must (err 1)))' > "$TMP_MUST2"
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_MUST2" 2>&1); rc=$?
rm -f "$TMP_MUST2"
if [ $rc -ne 0 ] && printf '%s' "$out" | grep -q "result-must: called on err"; then
    pass "eval-must-uncaught"
else
    fail "eval-must-uncaught" "expected nonzero + panic msg, got rc=$rc output: $out"
fi

# ---------------------------------------------------------------------------
# E4: Nonzero exit codes on errors (Tier 1 verification)
# ---------------------------------------------------------------------------

# tur check on a bad file exits nonzero
TMP_BAD=$(mktemp /tmp/tur_bad_XXXXXX.tur)
echo '(+ 1 "not-an-int")' > "$TMP_BAD"
"$TUR" check "$TMP_BAD" >/dev/null 2>&1; rc=$?
rm -f "$TMP_BAD"
if [ $rc -eq 0 ]; then
    fail "exit-code-check-error" "expected nonzero exit for type error, got 0"
else
    pass "exit-code-check-error"
fi

# tur check on a nonexistent file exits nonzero
"$TUR" check /nonexistent/file.tur >/dev/null 2>&1; rc=$?
if [ $rc -eq 0 ]; then
    fail "exit-code-file-not-found" "expected nonzero exit for missing file, got 0"
else
    pass "exit-code-file-not-found"
fi

# ---------------------------------------------------------------------------
# Tier 2: tur doc <sym>  (2a)
# ---------------------------------------------------------------------------

# tur doc +: should print arithmetic doc and exit 0
out=$("$TUR" doc "+" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-plus" "expected exit 0, got $rc; output: $out"
elif ! echo "$out" | grep -q "+"; then
    fail "doc-builtin-plus" "output '$out' did not contain '+'"
else
    pass "doc-builtin-plus"
fi

# tur doc let: should print let doc and exit 0
out=$("$TUR" doc "let" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-let" "expected exit 0, got $rc; output: $out"
elif ! echo "$out" | grep -q "let"; then
    fail "doc-builtin-let" "output '$out' did not contain 'let'"
else
    pass "doc-builtin-let"
fi

# tur doc defstruct: should print defstruct doc and exit 0
out=$("$TUR" doc "defstruct" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-defstruct" "expected exit 0, got $rc; output: $out"
elif ! echo "$out" | grep -q "defstruct"; then
    fail "doc-builtin-defstruct" "output '$out' did not contain 'defstruct'"
else
    pass "doc-builtin-defstruct"
fi

# tur doc unknown: should print error message and exit nonzero
out=$("$TUR" doc "this-symbol-does-not-exist-xyz" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "doc-unknown-sym" "expected nonzero exit for unknown symbol, got 0"
else
    pass "doc-unknown-sym"
fi

# tur doc --help: should print usage and exit 0
out=$("$TUR" doc --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-help" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur doc"; then
    fail "doc-help" "output '$out' did not mention 'tur doc'"
else
    pass "doc-help"
fi

# ---------------------------------------------------------------------------
# Tier 2: tur run -  (2b — stdin mode)
# ---------------------------------------------------------------------------

# tur run -: compile and run source from stdin
out=$(echo '(println (+ 40 2))' | "$TUR" run - 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "run-stdin" "expected exit 0, got $rc; output: $out"
elif ! echo "$out" | grep -q "42"; then
    fail "run-stdin" "expected '42' in output, got '$out'"
else
    pass "run-stdin"
fi

# ---------------------------------------------------------------------------
# Tier 3: tur explain subcommand  (E13)
# ---------------------------------------------------------------------------

# tur explain --help: should print usage and exit 0
out=$("$TUR" explain --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "explain-help" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "tur explain"; then
    fail "explain-help" "output '$out' did not mention 'tur explain'"
else
    pass "explain-help"
fi

# tur explain <snippet>: compile a bad snippet and exit nonzero
out=$("$TUR" explain '(+ 1 "x")' 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "explain-snippet-error" "expected nonzero exit for type error snippet, got 0"
else
    pass "explain-snippet-error"
fi

# tur explain <good snippet>: no errors, exit 0
out=$("$TUR" explain '(+ 1 2)' 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "explain-snippet-ok" "expected exit 0 for valid snippet, got $rc; output: $out"
else
    pass "explain-snippet-ok"
fi

# ---------------------------------------------------------------------------
# Tier 3: tur format --diff  (E12)
# ---------------------------------------------------------------------------

# tur format --diff on already-formatted file: exit 0, no output
TMP_FMT=$(mktemp /tmp/tur_fmt_XXXXXX.tur)
printf '(+ 1 2)\n' > "$TMP_FMT"
out=$("$TUR" format --diff "$TMP_FMT" 2>&1); rc=$?
rm -f "$TMP_FMT"
if [ $rc -ne 0 ]; then
    fail "format-diff-clean" "expected exit 0 for already-formatted file, got $rc; output: $out"
else
    pass "format-diff-clean"
fi

# tur format --diff on unformatted file: exit 1 and print diff
TMP_UGLY=$(mktemp /tmp/tur_ugly_XXXXXX.tur)
printf '(+   1   2)\n' > "$TMP_UGLY"
out=$("$TUR" format --diff "$TMP_UGLY" 2>&1); rc=$?
rm -f "$TMP_UGLY"
if [ $rc -eq 0 ]; then
    fail "format-diff-changed" "expected exit 1 for unformatted file, got 0"
elif ! echo "$out" | grep -q "^[-+]"; then
    fail "format-diff-changed" "expected diff output, got '$out'"
else
    pass "format-diff-changed"
fi

# tur format --help: should mention --diff
out=$("$TUR" format --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "format-help-diff" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q -- "--diff"; then
    fail "format-help-diff" "format --help did not mention --diff"
else
    pass "format-help-diff"
fi

# ---------------------------------------------------------------------------
# Tier 3: --json output flag  (E14)
# ---------------------------------------------------------------------------

# tur --json doc +: should print JSON with name and doc fields
out=$("$TUR" --json doc "+" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "json-doc" "expected exit 0, got $rc; output: $out"
elif ! echo "$out" | grep -q '"name"'; then
    fail "json-doc" "expected JSON with 'name' field, got '$out'"
elif ! echo "$out" | grep -q '"doc"'; then
    fail "json-doc" "expected JSON with 'doc' field, got '$out'"
else
    pass "json-doc"
fi

# tur --json doc unknown: exit 1 even in JSON mode
out=$("$TUR" --json doc "no-such-sym-xyz" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "json-doc-unknown" "expected nonzero exit for unknown sym in JSON mode, got 0"
else
    pass "json-doc-unknown"
fi

# ---------------------------------------------------------------------------
# CPS1 (cps-transform-plan): --dump-cps-coloring may-capture coloring
# ---------------------------------------------------------------------------

# dump-cps-coloring-partition: the analysis must color uses-shift (seed) and
# calls-shifter (transitive), and leave the pure functions + main uncolored.
# NOTE: match with bash globs, not `echo "$out" | grep -q`. The emit-c output is
# large; under `set -o pipefail` a matching `grep -q` exits early, SIGPIPEs the
# `echo`, and the pipeline is reported as failed even though the match succeeded.
CPS_FIXTURE="tests/fixtures/cps-coloring/input.tur"
out=$("$TUR" --dump-cps-coloring emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-coloring-partition" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-coloring: pure-arith uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'pure-arith uncolored'"
elif [[ "$out" != *"cps-coloring: also-pure uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'also-pure uncolored'"
elif [[ "$out" != *"cps-coloring: uses-shift COLORED"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'uses-shift COLORED' (seed)"
elif [[ "$out" != *"cps-coloring: calls-shifter COLORED"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'calls-shifter COLORED' (transitive)"
elif [[ "$out" != *"cps-coloring: main uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'main uncolored'"
else
    pass "dump-cps-coloring-partition"
fi

# dump-cps-coloring-no-output: without the flag, no coloring lines should appear.
out=$("$TUR" emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [[ "$out" == *"cps-coloring:"* ]]; then
    fail "dump-cps-coloring-no-output" "coloring dump appeared without --dump-cps-coloring flag"
else
    pass "dump-cps-coloring-no-output"
fi

# dump-cps-coloring-effects: F1 (compiled-first-class-continuations-plan) -- the
# algebraic-effect operators must seed the may-capture coloring the same way
# shift/reset do.  does-perform (direct `perform`) and has-handle (direct
# `handle`) are seeds; calls-performer reaches `perform` transitively; the two
# pure functions stay uncolored.  This locks the "mechanical part" of Phase F1.
EFF_FIXTURE="tests/fixtures/cps-effect-coloring/input.tur"
out=$("$TUR" --dump-cps-coloring emit-c "$EFF_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-coloring-effects" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-coloring: pure-arith uncolored"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'pure-arith uncolored'"
elif [[ "$out" != *"cps-coloring: also-pure uncolored"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'also-pure uncolored'"
elif [[ "$out" != *"cps-coloring: does-perform COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'does-perform COLORED' (perform seed)"
elif [[ "$out" != *"cps-coloring: calls-performer COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'calls-performer COLORED' (transitive)"
elif [[ "$out" != *"cps-coloring: has-handle COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'has-handle COLORED' (handle seed)"
else
    pass "dump-cps-coloring-effects"
fi

# ---------------------------------------------------------------------------
# CPS2 (cps-transform-plan): --dump-cps ANF/CPS IR
# ---------------------------------------------------------------------------
# (bash globs, not `grep -q` -- see the SIGPIPE/pipefail note above.)

# dump-cps-anf: the CPS IR for the colored functions must show ANF naming, the
# typed continuation parameter, a join continuation, a threaded tail call, and
# the reset/shift control forms.
out=$("$TUR" --dump-cps emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-anf" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-fn uses-shift"* ]]; then
    fail "dump-cps-anf" "expected a 'cps-fn uses-shift' block"
elif [[ "$out" != *"k:cont<int>"* ]]; then
    fail "dump-cps-anf" "expected typed continuation 'k:cont<int>'"
elif [[ "$out" != *"tailcall uses-shift("* ]]; then
    fail "dump-cps-anf" "expected a threaded 'tailcall uses-shift(...)'"
elif [[ "$out" != *"letcont j"* ]]; then
    fail "dump-cps-anf" "expected a join continuation 'letcont j...'"
elif [[ "$out" != *"reset __t"* ]]; then
    fail "dump-cps-anf" "expected a 'reset' control form"
elif [[ "$out" != *"shift k'"* ]]; then
    fail "dump-cps-anf" "expected a 'shift' control form"
else
    pass "dump-cps-anf"
fi

# dump-cps-no-output: without the flag, no CPS IR should appear.
out=$("$TUR" emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [[ "$out" == *"cps-fn "* ]]; then
    fail "dump-cps-no-output" "CPS IR dump appeared without --dump-cps flag"
else
    pass "dump-cps-no-output"
fi

# ---------------------------------------------------------------------------
# CPS3 (cps-transform-plan): selective lowering + direct<->CPS boundary bridging
# ---------------------------------------------------------------------------

# dump-cps-bridge: the mixed fixture must show the boundary classification --
# main is a direct->CPS entry root; shift-then-twice/run are internal; the call
# into the uncolored `twice` is a cps->direct bridge; colored tail calls are
# cps->cps; and the uncolored `twice` is NOT lowered (stays direct style).
MIX_FIXTURE="tests/fixtures/cps-mixed-coloring/input.tur"
out=$("$TUR" --dump-cps emit-c "$MIX_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-bridge" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-fn main "*"entry"* ]]; then
    fail "dump-cps-bridge" "expected main classified as a direct->CPS 'entry'"
elif [[ "$out" != *"cps-fn shift-then-twice "*"internal"* ]]; then
    fail "dump-cps-bridge" "expected shift-then-twice classified 'internal'"
elif [[ "$out" != *"call twice("*"; cps->direct"* ]]; then
    fail "dump-cps-bridge" "expected a 'cps->direct' bridge into uncolored twice"
elif [[ "$out" != *"; cps->cps"* ]]; then
    fail "dump-cps-bridge" "expected a 'cps->cps' threaded tail call"
elif [[ "$out" == *"cps-fn twice "* ]]; then
    fail "dump-cps-bridge" "uncolored twice must stay direct (not CPS-lowered)"
else
    pass "dump-cps-bridge"
fi

# ---------------------------------------------------------------------------
# try-turmeric-lang-toggle-plan T0: #lang layer toggle + canonical reader name
# ---------------------------------------------------------------------------

# lang-layer-toggle-off: within ONE interpreter session, `#lang turmeric
# stringed` must activate the #s"..." dispatch and a later `#lang turmeric`
# must genuinely deactivate it (the layer set is assigned, not accumulated,
# and turi_env_apply_lang wipes the session reader-macro registry). Before
# the fix the second #s"..." kept reading as a String.
out=$(printf '#lang turmeric stringed\n#s"on"\n#lang turmeric\n#s"off"\n:quit\n' \
      | "$TUR" repl 2>&1); rc=$?
if ! echo "$out" | grep -q '=> "on"'; then
    fail "lang-layer-toggle-off" "stringed layer did not activate (#s\"on\" not evaluated)"
elif echo "$out" | grep -q '=> "off"'; then
    fail "lang-layer-toggle-off" "#s\"...\" still dispatched after the layer was dropped"
elif ! echo "$out" | grep -q "unknown reader string macro '#s'"; then
    fail "lang-layer-toggle-off" "expected an unknown-reader-macro error once stringed is off"
else
    pass "lang-layer-toggle-off"
fi

# reader-name-canonical: reader_type_name(READER_SWEET) reports the canonical
# slash-namespaced spelling; the legacy `sweet-exp` alias is accepted on
# input but never generated, so the round-trip is stable.
out=$(printf '#lang sweet-exp\n#lang turmeric/sweet\n:quit\n' | "$TUR" repl 2>&1); rc=$?
if ! echo "$out" | grep -q "; reader set to turmeric/sweet (session reset)"; then
    fail "reader-name-canonical" "legacy alias did not report canonical 'turmeric/sweet'"
elif ! echo "$out" | grep -q "; reader already set to turmeric/sweet"; then
    fail "reader-name-canonical" "canonical spelling not recognized as the same reader"
elif echo "$out" | grep -q "reader (set to\|already set to) sweet-exp"; then
    fail "reader-name-canonical" "legacy 'sweet-exp' spelling was generated"
else
    pass "reader-name-canonical"
fi

# lang-layer-same-set-no-reset: repeating the SAME base+layer line must not
# reset the session (turi_env_apply_lang is a no-op when nothing changes).
out=$(printf '#lang turmeric stringed\n(def keep 41)\n#lang turmeric stringed\n(+ keep 1)\n:quit\n' \
      | "$TUR" repl 2>&1); rc=$?
if ! echo "$out" | grep -q "; reader already set to turmeric"; then
    fail "lang-layer-same-set-no-reset" "identical #lang line was not treated as a no-op"
elif ! echo "$out" | grep -q "=> 42"; then
    fail "lang-layer-same-set-no-reset" "binding did not survive an identical #lang line"
else
    pass "lang-layer-same-set-no-reset"
fi

# ---------------------------------------------------------------------------
# jit-ffi-c2mir-plan: dynamic FFI (call thunks, extern-c, call-ptr)
# ---------------------------------------------------------------------------

# The interpreter halves of the feature exist only in -DTUR_JIT=ON builds;
# probe the binary once and PASS-skip those cases against a JIT-less tur
# (mirroring how run-jit.sh treats an engine-less binary).  The non-JIT
# diagnostics ARE asserted either way.
TMP_FFI=$(mktemp -t tur-jit-ffi.XXXXXX.tur)
trap 'rm -f "$TMP_FFI"' EXIT
# NOTE: captured via command substitution, not a pipeline -- this script
# runs `set -o pipefail`, and `tur jit`'s non-zero exit (or the SIGPIPE from
# grep -q's early close) would mask a successful match.
HAS_JIT=1
printf '(defn main [] : int 0)\n' > "$TMP_FFI"
probe_out=$("$TUR" jit "$TMP_FFI" 2>&1 || true)
case "$probe_out" in *"no JIT engine"*) HAS_JIT=0 ;; esac

# jit-ffi-extern-c-real: under --interpret in a JIT build, an extern-c
# declaration beyond the known-override table resolves via dlsym and gets
# CALLED for real -- the old behavior silently returned nil (printed 0).
printf '(extern-c strtol [s :cstr endp :int base :int] :int)\n(defn main [] : int (println (strtol "123abc" 0 10)) 0)\n' > "$TMP_FFI"
if [ "$HAS_JIT" = "1" ]; then
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if ! echo "$out" | grep -q "^123$"; then
        fail "jit-ffi-extern-c-real" "expected strtol to return 123 under --interpret, got: $out"
    else
        pass "jit-ffi-extern-c-real"
    fi
else
    echo "SKIP jit-ffi-extern-c-real (no JIT engine in this build)"
fi

# jit-ffi-call-ptr-interp: the call-ptr form routed through the c2mir thunk
# provider under --interpret (dlopen -> dlsym -> call-ptr, the full loop).
# Linux-only: the fixture names a soname; macOS spells libm differently and
# the portable half is covered by tests/fixtures/jit-ffi-call-ptr.
if [ "$HAS_JIT" = "1" ] && [ "$(uname)" = "Linux" ]; then
    printf '(defn main [] : int\n  (unsafe\n    (let [h (dlopen "libm.so.6")\n          p (dlsym h "cbrt")]\n      (println (call-ptr p [:float -> :float] 27.0))))\n  0)\n' > "$TMP_FFI"
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --enable=jit-ffi --interpret "$TMP_FFI" 2>/dev/null)
    if ! echo "$out" | grep -q "^3$"; then
        fail "jit-ffi-call-ptr-interp" "expected cbrt(27) = 3 via call-ptr under --interpret, got: $out"
    else
        pass "jit-ffi-call-ptr-interp"
    fi
else
    echo "SKIP jit-ffi-call-ptr-interp (needs a JIT build on Linux)"
fi

# jit-ffi-call-ptr-nonjit-diag: a JIT-less interpreter reports a clean
# "requires a JIT-enabled build" diagnostic for call-ptr -- never nil, never
# a crash.  (In a JIT build the call succeeds instead, so only the JIT-less
# side of the fork is asserted here.)
if [ "$HAS_JIT" = "0" ]; then
    printf '(defn main [] : int\n  (unsafe (println (call-ptr 1 [-> :int])))\n  0)\n' > "$TMP_FFI"
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --enable=jit-ffi --interpret "$TMP_FFI" 2>&1)
    if ! echo "$out" | grep -q "requires a JIT-enabled build"; then
        fail "jit-ffi-call-ptr-nonjit-diag" "expected the clean non-JIT diagnostic, got: $out"
    else
        pass "jit-ffi-call-ptr-nonjit-diag"
    fi
else
    echo "SKIP jit-ffi-call-ptr-nonjit-diag (this build has the engine)"
fi

# jit-ffi-call-ptr-struct-interp: F4 struct-by-value through the interpreter's
# own marshaller (TuriStruct -> C bytes -> TuriStruct), for the aggregate
# classes every backend agrees on: all-integer, and mixed int/float.  Covers
# both an aggregate ARGUMENT and an aggregate RETURN.  The compiled half --
# including the floating-point aggregate the interpreter refuses below -- is
# tests/fixtures/jit-ffi-call-ptr-struct.
#
# `div` is the portable choice of callee: every libc declares it, and it
# RETURNS div_t {int quot; int rem;} by value -- an all-integer aggregate.
# Only the soname differs per platform (turi's dlopen has no spelling for
# "this process", so the library is named explicitly).
case "$(uname)" in
  Darwin) LIBC_SO="/usr/lib/libSystem.B.dylib" ;;
  Linux)  LIBC_SO="libc.so.6" ;;
  *)      LIBC_SO="" ;;
esac
if [ "$HAS_JIT" = "1" ] && [ -n "$LIBC_SO" ]; then
    cat > "$TMP_FFI" <<TURFFI
(defstruct IPair [a : int32 b : int32])
(defn main [] : int
  (unsafe
    (let [h  (dlopen "$LIBC_SO")
          pd (dlsym h "div")]
      (let [r (call-ptr pd [:int :int -> IPair] 47 10)]
        (println (:: (.a r) :int))
        (println (:: (.b r) :int)))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --enable=jit-ffi --interpret "$TMP_FFI" 2>/dev/null)
    if [ "$(echo "$out" | tr '\n' ' ')" != "4 7 " ]; then
        fail "jit-ffi-call-ptr-struct-interp" "expected div(47,10) = {4,7} unpacked from an aggregate return, got: $out"
    else
        pass "jit-ffi-call-ptr-struct-interp"
    fi
else
    echo "SKIP jit-ffi-call-ptr-struct-interp (needs a JIT build on a known libc)"
fi

# jit-ffi-call-ptr-hfa-refused: on aarch64 the interpreter must REFUSE a
# floating-point aggregate rather than emit a thunk that mis-passes it --
# MIR has no HFA class and would put it in x0..x7 where a natively compiled
# callee reads v0..v7.  See docs/reported/mir-aarch64-fp-aggregate-abi.md.
# The failure mode this guards against is a silent wrong ANSWER, so the
# assertion is that the diagnostic appears, not that the call fails.
case "$(uname -m)" in
  arm64|aarch64) HFA_HOST=1 ;;
  *)             HFA_HOST=0 ;;
esac
if [ "$HAS_JIT" = "1" ] && [ "$HFA_HOST" = "1" ]; then
    cat > "$TMP_FFI" <<'TURFFI'
(defstruct Vec2 [x : float y : float])
(defn main [] : int
  (unsafe (println (call-ptr 1 [Vec2 -> :float] (Vec2 3.0 4.0))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --enable=jit-ffi --interpret "$TMP_FFI" 2>&1)
    if ! echo "$out" | grep -q "AAPCS64 HFA"; then
        fail "jit-ffi-call-ptr-hfa-refused" "expected the aarch64 HFA refusal, got: $out"
    else
        pass "jit-ffi-call-ptr-hfa-refused"
    fi
else
    echo "SKIP jit-ffi-call-ptr-hfa-refused (needs a JIT build on aarch64)"
fi

# jit-ffi-gate: without --enable=jit-ffi the form is a hard error pointing
# at the experiment, on every build.
printf '(defn main [] : int\n  (unsafe (println (call-ptr 1 [-> :int])))\n  0)\n' > "$TMP_FFI"
out=$("$TUR" emit-c "$TMP_FFI" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "jit-ffi-gate" "call-ptr compiled without --enable=jit-ffi"
elif ! echo "$out" | grep -q "enable=jit-ffi"; then
    fail "jit-ffi-gate" "gate diagnostic did not point at --enable=jit-ffi"
else
    pass "jit-ffi-gate"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "flags summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
