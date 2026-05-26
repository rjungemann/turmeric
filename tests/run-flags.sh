#!/usr/bin/env bash
# tests/run-flags.sh — Tests for global flags that can't be expressed as
# standard fixture tests (--explain, --dump-kinds, etc.)
#
# Each test case is a short bash block that prints "PASS <name>" on success
# or "FAIL <name> — <reason>" on failure, then increments the counerr?.
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
            TUR-E0007 TUR-E0009 TUR-E0010 TUR-E0011 TUR-E0012 TUR-E0013 TUR-E0021; do
    out=$("$TUR" --explain "$code" 2>&1); rc=$?
    if [ $rc -ne 0 ] || [ -z "$out" ]; then
        fail "tur-explain-${code}" "no explanation regiserr?ed (exit=$rc)"
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
# should print "defclass Funcok?r param[0] : * -> *" ok? stdout.
FIXTURE="tests/fixtures/dump-kinds-basic/input.tur"
out=$("$TUR" --dump-kinds emit-c "$FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-kinds-basic" "non-zero exit ($rc)"
elif [[ "$out" != *"defclass Funcok?r param[0] : * -> *"* ]]; then
    fail "dump-kinds-basic" "expected 'defclass Funcok?r param[0] : * -> *' in output"
else
    pass "dump-kinds-basic"
fi

# dump-kinds-no-output: without --dump-kinds, the same file should NOT print
# kind annotations mixed inok? the C output.
out=$("$TUR" emit-c "$FIXTURE" 2>/dev/null); rc=$?
if echo "$out" | grep -q "defclass Funcok?r param"; then
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
# Formaerr? round-trip tests (defpackage / deflockfile / comment preservation)
# ---------------------------------------------------------------------------

# fmt-defpackage-basic: a defpackage wide enough ok? exceed line width should
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
    fail "fmt-defpackage-round-trip" "formaerr? not idempotent; first pass != second pass"
else
    pass "fmt-defpackage-round-trip"
fi

# fmt-defpackage-spices-block: a defpackage with multiple spices should expand
# the :spices map ok? block layout when it doesn't fit on one line.
_fmt_spices_in='(defpackage my-application :name "my-application" :version "0.1.0" :spices #{"geom" #{ :url "https://github.com/alice/tur-geom" :ref "v0.2.1"} "http" #{ :url "https://github.com/alice/tur-http" :ref "v1.0.0"}})'
_fmt_spices_out=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-spices-block" "tur format exited $rc"
elif ! echo "$_fmt_spices_out" | grep -q '#{$'; then
    fail "fmt-defpackage-spices-block" ":spices map was not expanded ok? block: $_fmt_spices_out"
else
    pass "fmt-defpackage-spices-block"
fi

# fmt-defpackage-spices-idempotent: the spices block output is itself idempotent.
_fmt_spices1=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1)
_fmt_spices2=$(echo "$_fmt_spices1" | "$TUR" format 2>&1)
if [ "$_fmt_spices1" != "$_fmt_spices2" ]; then
    fail "fmt-defpackage-spices-idempotent" "formaerr? not idempotent for spices block"
else
    pass "fmt-defpackage-spices-idempotent"
fi

# fmt-defpackage-comments: a defpackage with a leading comment should preserve
# the comment through a format round-trip.
_fmt_cmt_out=$(printf '; This is a comment\n(defpackage my-app\n  :name    "my-app"\n  :version "0.1.0")' | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-comments" "tur format exited $rc: $_fmt_cmt_out"
elif ! echo "$_fmt_cmt_out" | grep -q 'This is a comment'; then
    fail "fmt-defpackage-comments" "comment lost aferr? formatting: $_fmt_cmt_out"
else
    pass "fmt-defpackage-comments"
fi

# fmt-deflockfile-round-trip: deflockfile form should be idempotent.
_fmt_lock1=$(printf '(deflockfile\n  :format-version 1\n  :spices         #{}\n  :cmake-deps     #{})' | "$TUR" format 2>&1)
_fmt_lock2=$(echo "$_fmt_lock1" | "$TUR" format 2>&1)
if [ "$_fmt_lock1" != "$_fmt_lock2" ]; then
    fail "fmt-deflockfile-round-trip" "formaerr? not idempotent for deflockfile"
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
# Note: use a temp file ok? avoid grep -q SIGPIPE issue with large output + pipefail.
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

# TS1: float closures returned from a call head should keep their concrete
# thunk result type, and emit-c should not require union bit-casts.
TYPED_SLOTS_FIXTURE="tests/fixtures/typed-slots/float-closure/input.tur"
_typed_slots_out=$(mktemp /tmp/tur-typed-slots-XXXXXX)
"$TUR" emit-c "$TYPED_SLOTS_FIXTURE" > "$_typed_slots_out" 2>/dev/null; rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-float-closure-codegen" "emit-c exited $rc"
elif ! grep -Eq 'typedef double \(\*tur_thunk_double_double_t\)\(void \*, double\);' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "expected shared typed thunk typedef in emitted C"
elif ! grep -Eq 'struct __env_[0-9]+ \{ tur_thunk_double_double_t __fn; double a; \};' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "expected fat closure __fn field to use the typed thunk typedef"
elif ! grep -Eq 'static double __fn_[0-9]+\(void \*, double\);' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "expected concrete double thunk signature in emitted C"
elif ! grep -Eq '__fn_[0-9]+\(__call_head_[0-9_]+, 2\.25\);' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "expected direct typed thunk call for returned closure"
elif grep -Eq 'struct __env_[0-9]+ \{ int64_t __fn;' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "unexpected legacy int64_t __fn field remained in the fat closure layout"
elif grep -Eq 'union \{ int64_t i; double d; \}|union \{ double d; int64_t i; \}' "$_typed_slots_out"; then
    fail "typed-slots-float-closure-codegen" "unexpected float/int64 union bit-cast found in emitted C"
else
    pass "typed-slots-float-closure-codegen"
fi
rm -f "$_typed_slots_out"

# GS1/GS2: parameterized defstruct field types should accept both bare and
# spaced type-parameter spellings, and make-struct / field access / set! should
# preserve the instantiated field types through elaboration.
_generic_struct_in=$(mktemp /tmp/tur-generic-struct-XXXXXX.tur)
cat > "$_generic_struct_in" <<'EOF'
(defstruct Box [A]
  (value A))

(defstruct Duo [A B]
  (fst : A)
  (snd : B))

(defn main [] :int
  (let [b (make-struct Box 3.5)
        ^mut p (make-struct Duo 1.25 "ok")]
    (println (+ (.value b) 0.25))
    (set! (.fst p) 2.75)
    (println (.fst p))
    (println (.snd p))
    0))
EOF
out=$("$TUR" check "$_generic_struct_in" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-struct-check" "tur check failed: $out"
else
    pass "typed-slots-generic-struct-check"
fi
rm -f "$_generic_struct_in"

_generic_struct_calls_ok=$(mktemp /tmp/tur-generic-struct-calls-ok-XXXXXX.tur)
cat > "$_generic_struct_calls_ok" <<'EOF'
(defstruct Pair2 [A B]
  (fst A)
  (snd B))

(defn takes-float [x :float] :float x)

(defn main [] :int
  (let [p (:: (make-struct Pair2 1 2.5) (Pair2 int float))]
    (takes-float (.snd p))
    0))
EOF
out=$("$TUR" check "$_generic_struct_calls_ok" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-struct-call-check" "tur check failed: $out"
else
    pass "typed-slots-generic-struct-call-check"
fi
rm -f "$_generic_struct_calls_ok"

_generic_struct_calls_bad=$(mktemp /tmp/tur-generic-struct-calls-bad-XXXXXX.tur)
cat > "$_generic_struct_calls_bad" <<'EOF'
(defstruct Box [A]
  (value A))

(defn need-float-box [b :(Box float)] :float
  (.value b))

(defn main [] :int
  (let [b (:: (make-struct Box 3) (Box int))]
    (need-float-box b)
    0))
EOF
if out=$("$TUR" check "$_generic_struct_calls_bad" 2>&1); then
    fail "typed-slots-generic-struct-call-mismatch" "expected type mismatch for (Box int) passed to (Box float)"
elif ! echo "$out" | grep -E "(type-app Box float|\\(Box float\\))" > /dev/null 2>&1; then
    fail "typed-slots-generic-struct-call-mismatch" "expected diagnostic to mention Box float, got: $out"
else
    pass "typed-slots-generic-struct-call-mismatch"
fi
rm -f "$_generic_struct_calls_bad"

_generic_struct_layout_out=$(mktemp /tmp/tur-generic-struct-layout-XXXXXX.c)
_generic_struct_layout_fixture="tests/fixtures/typed-slots/generic-struct-layout/input.tur"
"$TUR" emit-c "$_generic_struct_layout_fixture" > "$_generic_struct_layout_out" 2>/dev/null; rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-struct-layout" "emit-c exited $rc"
elif ! grep -Eq 'typedef struct Box__float \{' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected Box__float typedef in emitted C"
elif ! grep -Eq 'typedef struct Duo2__int__float \{' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected Duo2__int__float typedef in emitted C"
elif ! grep -Eq 'double value;' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected concrete double field for Box__float"
elif ! grep -Eq 'int64_t fst;' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected concrete int64_t fst field for Duo2__int__float"
elif ! grep -Eq 'double snd;' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected concrete double snd field for Duo2__int__float"
elif ! grep -Eq 'static Box__float id_box\(Box__float b\)' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected function signatures to use Box__float"
elif ! grep -Eq 'Duo2__int__float p_[0-9]+ = \(Duo2__int__float\)\{' "$_generic_struct_layout_out"; then
    fail "typed-slots-generic-struct-layout" "expected make-struct to use Duo2__int__float compound literals"
else
    pass "typed-slots-generic-struct-layout"
fi
rm -f "$_generic_struct_layout_out"

_generic_fn_binders_fixture="tests/fixtures/typed-slots/generic-fn-binders/input.tur"
out=$("$TUR" check "$_generic_fn_binders_fixture" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-fn-binders-check" "tur check failed: $out"
else
    pass "typed-slots-generic-fn-binders-check"
fi

out=$("$TUR" run "$_generic_fn_binders_fixture" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-fn-binders-run" "tur run exited $rc"
elif [ "$out" != "$(cat tests/fixtures/typed-slots/generic-fn-binders/expected.stdout)" ]; then
    fail "typed-slots-generic-fn-binders-run" "unexpected output: '$out'"
else
    pass "typed-slots-generic-fn-binders-run"
fi

_generic_fn_nested=$(mktemp /tmp/tur-generic-fn-nested-XXXXXX.tur)
cat > "$_generic_fn_nested" <<'EOF'
(defstruct Box2 [A]
  (value A))

(defstruct Wrap2 [A]
  (inner A))

(defn id-wrap [A] [w :(Wrap2 (Box2 A))] :(Wrap2 (Box2 A))
  w)

(defn main [] :int 0)
EOF
out=$("$TUR" check "$_generic_fn_nested" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-fn-nested-signature" "tur check failed: $out"
else
    pass "typed-slots-generic-fn-nested-signature"
fi
rm -f "$_generic_fn_nested"

_generic_fn_nested_use=$(mktemp /tmp/tur-generic-fn-nested-use-XXXXXX.tur)
cat > "$_generic_fn_nested_use" <<'EOF'
(defstruct Box2 [A]
  (value A))

(defstruct Wrap2 [A]
  (inner A))

(defn id-wrap [A] [w :(Wrap2 (Box2 A))] :(Wrap2 (Box2 A))
  w)

(defn takes-float [x :float] :float x)

(defn main [] :int
  (let [w (:: (make-struct Wrap2 (:: (make-struct Box2 3.5) (Box2 float)))
              (Wrap2 (Box2 float)))]
    (takes-float (.value (.inner (id-wrap w))))
    0))
EOF
out=$("$TUR" check "$_generic_fn_nested_use" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-fn-nested-use" "tur check failed: $out"
else
    pass "typed-slots-generic-fn-nested-use"
fi
rm -f "$_generic_fn_nested_use"

_pair_macro_fixture=$(mktemp /tmp/tur-pair-macro-XXXXXX.tur)
cat > "$_pair_macro_fixture" <<'EOF'
(defstruct Pair2 [A B]
  (fst A)
  (snd B))

(defmacro pair-fst [p]
  (let [accessor-sym fst]
    (list (dot-sym accessor-sym) p)))

(defn main [] :int
  (let [p (:: (make-struct Pair2 1 2.5) (Pair2 int float))]
    (println (pair-fst p))
    0))
EOF
out=$("$TUR" check "$_pair_macro_fixture" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-pair-macro-access" "tur check failed: $out"
else
    out=$("$TUR" run "$_pair_macro_fixture" 2>/dev/null); rc=$?
    if [ $rc -ne 0 ]; then
        fail "typed-slots-pair-macro-access" "tur run exited $rc"
    elif [ "$out" != "1" ]; then
        fail "typed-slots-pair-macro-access" "unexpected output: '$out'"
    else
        pass "typed-slots-pair-macro-access"
    fi
fi
rm -f "$_pair_macro_fixture"

_generic_helper_fixture=$(mktemp /tmp/tur-generic-helper-XXXXXX)
_generic_helper_c=$(mktemp /tmp/tur-generic-helper-c-XXXXXX)
cat > "$_generic_helper_fixture" <<'EOF'
(defstruct Box [A]
  (value A))

(defn mk-box [A] [x :A] :(Box A)
  (make-struct Box x))

(defn get-box [A] [b :(Box A)] :A
  (.value b))

(defn main [] :int
  (let [b (mk-box 3.5)]
    (println (get-box b))
    0))
EOF
out=$("$TUR" emit-c "$_generic_helper_fixture" >"$_generic_helper_c" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-generic-helper-specialization" "emit-c exited $rc"
elif ! grep -q 'mk_box__spec__Box__float_double' "$_generic_helper_c"; then
    fail "typed-slots-generic-helper-specialization" "expected concrete mk-box specialization in emitted C"
elif ! grep -q 'get_box__spec__double_Box__float' "$_generic_helper_c"; then
    fail "typed-slots-generic-helper-specialization" "expected concrete get-box specialization in emitted C"
elif ! grep -q 'mk_box__spec__Box__float_double(3.5)' "$_generic_helper_c"; then
    fail "typed-slots-generic-helper-specialization" "expected call site to use specialized mk-box clone"
elif ! grep -q 'get_box__spec__double_Box__float(b_' "$_generic_helper_c"; then
    fail "typed-slots-generic-helper-specialization" "expected call site to use specialized get-box clone"
else
    out=$("$TUR" run "$_generic_helper_fixture" 2>/dev/null); rc=$?
    if [ $rc -ne 0 ]; then
        fail "typed-slots-generic-helper-specialization" "tur run exited $rc"
    elif [ "$out" != "3.5" ]; then
        fail "typed-slots-generic-helper-specialization" "unexpected output: '$out'"
    else
        pass "typed-slots-generic-helper-specialization"
    fi
fi
rm -f "$_generic_helper_fixture"
rm -f "$_generic_helper_c"

_pair_second_fixture=$(mktemp /tmp/tur-pair-second-XXXXXX)
_pair_second_c=$(mktemp /tmp/tur-pair-second-c-XXXXXX)
cat > "$_pair_second_fixture" <<'EOF'
(defstruct Pair2 [A B]
  (fst A)
  (snd B))

(defn pair-second [A B] [p :(Pair2 A B)] :B
  (.snd p))

(defn main [] :int
  (let [p (:: (make-struct Pair2 1 2.5) (Pair2 int float))]
    (println (pair-second p))
    0))
EOF
out=$("$TUR" emit-c "$_pair_second_fixture" >"$_pair_second_c" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-pair-second-specialization" "emit-c exited $rc"
elif ! grep -q 'pair_second__spec__double_Pair2__int__float' "$_pair_second_c"; then
    fail "typed-slots-pair-second-specialization" "expected concrete pair-second specialization in emitted C"
elif ! grep -q 'pair_second__spec__double_Pair2__int__float(p_' "$_pair_second_c"; then
    fail "typed-slots-pair-second-specialization" "expected call site to use specialized pair-second clone"
else
    out=$("$TUR" run "$_pair_second_fixture" 2>/dev/null); rc=$?
    if [ $rc -ne 0 ]; then
        fail "typed-slots-pair-second-specialization" "tur run exited $rc"
    elif [ "$out" != "2.5" ]; then
        fail "typed-slots-pair-second-specialization" "unexpected output: '$out'"
    else
        pass "typed-slots-pair-second-specialization"
    fi
fi
rm -f "$_pair_second_fixture"
rm -f "$_pair_second_c"

_pair_helpers_fixture=$(mktemp /tmp/tur-pair-helpers-XXXXXX)
_pair_helpers_c=$(mktemp /tmp/tur-pair-helpers-c-XXXXXX)
cat > "$_pair_helpers_fixture" <<'EOF'
(defn main [] :int
  (let [p (:: (tpair 1 2.5) (Pair int float))
        r (:: (tpair 1 2) (Pair int int))
        s (:: (tpair 1 2) (Pair int int))]
    (println (pair-fst p))
    (println (pair-snd p))
    (println (pair-eq? r s (fn [a b] (= a b)) (fn [a b] (= a b))))
    0))
EOF
out=$("$TUR" emit-c "$_pair_helpers_fixture" >"$_pair_helpers_c" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-pair-helpers" "emit-c exited $rc"
elif ! grep -q 'Pair__int__float' "$_pair_helpers_c"; then
    fail "typed-slots-pair-helpers" "expected Pair__int__float layout in emitted C"
elif ! grep -q 'pair_fst__spec__int64_t_Pair__int__float' "$_pair_helpers_c"; then
    fail "typed-slots-pair-helpers" "expected specialized pair-fst helper in emitted C"
elif ! grep -q 'pair_snd__spec__double_Pair__int__float' "$_pair_helpers_c"; then
    fail "typed-slots-pair-helpers" "expected specialized pair-snd helper in emitted C"
else
    out=$("$TUR" run "$_pair_helpers_fixture" 2>/dev/null); rc=$?
    if [ $rc -ne 0 ]; then
        fail "typed-slots-pair-helpers" "tur run exited $rc"
    elif [ "$out" != "$(printf '1\n2.5\ntrue')" ]; then
        fail "typed-slots-pair-helpers" "unexpected output: '$out'"
    else
        pass "typed-slots-pair-helpers"
    fi
fi
rm -f "$_pair_helpers_fixture"
rm -f "$_pair_helpers_c"

_list_helpers_fixture=$(mktemp /tmp/tur-list-helpers-XXXXXX)
_list_helpers_c=$(mktemp /tmp/tur-list-helpers-c-XXXXXX)
cat > "$_list_helpers_fixture" <<'EOF'
(defn takes-float [x :float] :float x)

(defn main [] :int
  (let [xs (:: (make-struct Cons 3.5 (tnil)) (Cons float))]
    (println (takes-float (thead xs)))
    (println (tnil? (ttail xs)))
    0))
EOF
out=$("$TUR" emit-c "$_list_helpers_fixture" >"$_list_helpers_c" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-list-helpers" "emit-c exited $rc"
elif ! grep -q 'thead__spec__double_Cons__float' "$_list_helpers_c"; then
    fail "typed-slots-list-helpers" "expected specialized thead helper in emitted C"
elif ! grep -q 'ttail__spec__int64_t_Cons__float' "$_list_helpers_c"; then
    fail "typed-slots-list-helpers" "expected specialized ttail helper in emitted C"
else
    out=$("$TUR" run "$_list_helpers_fixture" 2>/dev/null); rc=$?
    if [ $rc -ne 0 ]; then
        fail "typed-slots-list-helpers" "tur run exited $rc"
    elif [ "$out" != "$(printf '3.5\ntrue')" ]; then
        fail "typed-slots-list-helpers" "unexpected output: '$out'"
    else
        pass "typed-slots-list-helpers"
    fi
fi
rm -f "$_list_helpers_fixture"
rm -f "$_list_helpers_c"

_stdlib_container_fixture="tests/fixtures/typed-slots/stdlib-container-layout/input.tur"
out=$("$TUR" check "$_stdlib_container_fixture" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-stdlib-container-check" "tur check failed: $out"
else
    pass "typed-slots-stdlib-container-check"
fi

out=$("$TUR" run "$_stdlib_container_fixture" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-stdlib-container-run" "tur run exited $rc"
elif [ "$out" != "$(cat tests/fixtures/typed-slots/stdlib-container-layout/expected.stdout)" ]; then
    fail "typed-slots-stdlib-container-run" "unexpected output: '$out'"
else
    pass "typed-slots-stdlib-container-run"
fi

_stdlib_container_layout_out=$(mktemp /tmp/tur-stdlib-container-layout-XXXXXX.c)
"$TUR" emit-c "$_stdlib_container_fixture" > "$_stdlib_container_layout_out" 2>/dev/null; rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-stdlib-container-layout" "emit-c exited $rc"
elif ! grep -Eq 'typedef struct Option__float \{' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Option__float typedef in emitted C"
elif ! grep -Eq 'typedef struct Pair__int__float \{' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Pair__int__float typedef in emitted C"
elif ! grep -Eq 'typedef struct Result__float__cstr \{' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Result__float__cstr typedef in emitted C"
elif ! grep -Eq 'bool is_some;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Option__float to retain the bool tag"
elif ! grep -Eq 'double value;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Option__float to use a concrete double payload"
elif ! grep -Eq 'int64_t fst;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Pair__int__float fst to stay int64_t"
elif ! grep -Eq 'double snd;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Pair__int__float snd to use a concrete double payload"
elif ! grep -Eq 'double ok_val;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Result__float__cstr ok_val to use a concrete double payload"
elif ! grep -Eq 'const char \* err_val;' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected Result__float__cstr err_val to use a concrete cstr payload"
elif ! grep -Eq 'unwrap__spec__double_Option__float' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected specialized unwrap helper in emitted C"
elif ! grep -Eq 'ok_val__spec__double_Result__float__cstr' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected specialized ok-val helper in emitted C"
elif ! grep -Eq 'err_val__spec__const_char___Result__float__cstr' "$_stdlib_container_layout_out"; then
    fail "typed-slots-stdlib-container-layout" "expected specialized err-val helper in emitted C"
else
    pass "typed-slots-stdlib-container-layout"
fi
rm -f "$_stdlib_container_layout_out"

_cons_float_fixture="tests/fixtures/typed-slots/cons-float-layout/input.tur"
out=$("$TUR" check "$_cons_float_fixture" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-cons-float-check" "tur check failed: $out"
else
    pass "typed-slots-cons-float-check"
fi

out=$("$TUR" run "$_cons_float_fixture" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-cons-float-run" "tur run exited $rc"
elif [ "$out" != "$(cat tests/fixtures/typed-slots/cons-float-layout/expected.stdout)" ]; then
    fail "typed-slots-cons-float-run" "unexpected output: '$out'"
else
    pass "typed-slots-cons-float-run"
fi

_cons_float_layout_out=$(mktemp /tmp/tur-cons-float-layout-XXXXXX.c)
"$TUR" emit-c "$_cons_float_fixture" > "$_cons_float_layout_out" 2>/dev/null; rc=$?
if [ $rc -ne 0 ]; then
    fail "typed-slots-cons-float-layout" "emit-c exited $rc"
elif ! grep -Eq 'typedef struct Cons__float \{' "$_cons_float_layout_out"; then
    fail "typed-slots-cons-float-layout" "expected Cons__float typedef in emitted C"
elif ! grep -Eq 'double head;' "$_cons_float_layout_out"; then
    fail "typed-slots-cons-float-layout" "expected Cons__float head to use a concrete double slot"
elif ! grep -Eq 'int64_t tail;' "$_cons_float_layout_out"; then
    fail "typed-slots-cons-float-layout" "expected Cons__float tail to remain the carrier link"
elif ! grep -Eq 'Cons__float xs_[0-9]+ = \(Cons__float\)\{\.head = 1\.5, \.tail = INT64_C\(0\)\};' "$_cons_float_layout_out"; then
    fail "typed-slots-cons-float-layout" "expected make-struct Cons to lower to a Cons__float compound literal"
else
    pass "typed-slots-cons-float-layout"
fi
rm -f "$_cons_float_layout_out"

# try-with-basic: try-with behaves identically ok? handle
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

# effect-export-syntax: (export (effect Write)) is accepted by the parser/elab
out=$("$TUR" run tests/fixtures/effect-export-explicit/input.tur 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "effect-export-syntax" "should compile (exit=$rc): $out"
elif [ "$out" != "hello" ]; then
    fail "effect-export-syntax" "unexpected output: '$out'"
else
    pass "effect-export-syntax"
fi

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

# tur --version: should print "turmeric <version>" and exit 0
out=$("$TUR" --version 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-long" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "^turmeric "; then
    fail "version-long" "output '$out' did not start with 'turmeric '"
else
    pass "version-long"
fi

# tur -V: same as --version
out=$("$TUR" -V 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-short" "expected exit 0, got $rc"
elif ! echo "$out" | grep -q "^turmeric "; then
    fail "version-short" "output '$out' did not start with 'turmeric '"
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

# tur eval --file: run a .tur file through the inerr?preerr?
TMP_TUR=$(mktemp /tmp/tur_eval_test_XXXXXX.tur)
echo '(def x 42) (+ x 1)' > "$TMP_TUR"
out=$("$TUR" eval --file "$TMP_TUR" 2>&1); rc=$?
rm -f "$TMP_TUR"
if [ $rc -ne 0 ]; then
    fail "eval-file" "expected exit 0, got $rc; output: $out"
else
    pass "eval-file"
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
# Summary
# ---------------------------------------------------------------------------
echo
echo "flags summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
