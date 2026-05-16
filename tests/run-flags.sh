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

TUR="./build/tur"
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
            TUR-E0007 TUR-E0009 TUR-E0010 TUR-E0011 TUR-E0012 TUR-E0013; do
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
out=$("$TUR" run tests/fixtures/try-with-basic/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-basic" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'asking\n42')" ]; then
    fail "try-with-basic" "unexpected output: '$out'"
else
    pass "try-with-basic"
fi

# try-with-nested: nested try-with handlers work correctly
out=$("$TUR" run tests/fixtures/try-with-nested/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-nested" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'start\noops')" ]; then
    fail "try-with-nested" "unexpected output: '$out'"
else
    pass "try-with-nested"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "flags summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
