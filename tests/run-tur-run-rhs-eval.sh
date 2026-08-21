#!/usr/bin/env bash
# run-tur-run-rhs-eval.sh -- regression for tur-run-just-assignment-eval-plan.
#
# `tur run` used to store Justfile `name := VALUE` assignments as raw byte
# fragments (truncated at whitespace) and interpolate them verbatim, which
# made any Justfile whose RHS was more than a bare word / quoted string
# unusable. This test drives the balanced RHS scanner + expression
# evaluator: function calls, string concat via '/' and '+', `if/else`
# conditionals, nested calls, forward-ref errors, and interop with
# environment variables.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

case "$(uname -s)" in
  Darwin) HOST_OS=macos ;;
  Linux)  HOST_OS=linux ;;
  *)      HOST_OS=unknown ;;
esac

# ---------------------------------------------------------------------------
# 1. Balanced-parens RHS: multi-arg function call with a comma survives the
#    scanner and evaluates through eval_builtin.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
val := env_var_or_default("TUR_RUN_TEST_UNSET", "fallback-value")

show:
    @echo "val={{val}}"
EOF
unset TUR_RUN_TEST_UNSET
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'val=fallback-value' <<< "$OUT"; then
  pass "balanced-parens: env_var_or_default default is honored"
else
  fail "balanced-parens: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 2. Slash concat: "build" / "debug" yields "build/debug".
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
d := "build" / "debug"

show:
    @echo "d={{d}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'd=build/debug' <<< "$OUT"; then
  pass "slash-concat: 'build' / 'debug' -> build/debug"
else
  fail "slash-concat: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 3. Plus concat: "foo" + "bar" yields "foobar".
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
s := "foo" + "bar"

show:
    @echo "s={{s}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 's=foobar' <<< "$OUT"; then
  pass "plus-concat: 'foo' + 'bar' -> foobar"
else
  fail "plus-concat: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 4. Conditional expression: if os() == "<host>" { "yes" } else { "no" }.
# ---------------------------------------------------------------------------
cat > Justfile <<EOF
p := if os() == "${HOST_OS}" { "yes" } else { "no" }

show:
    @echo "p={{p}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'p=yes' <<< "$OUT"; then
  pass "conditional: matched host OS branch"
else
  fail "conditional: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 5. Nested case (the scite motivating example distilled): a conditional as
#    the default arg to env_var_or_default, with the outer var interpolated
#    through another assignment that uses '/'.
# ---------------------------------------------------------------------------
cat > Justfile <<EOF
preset := env_var_or_default("TUR_RUN_TEST_PRESET_UNSET", if os() == "${HOST_OS}" { "host-debug" } else { "other-debug" })
build_dir := "build" / preset

show:
    @echo "preset={{preset}} build_dir={{build_dir}}"
EOF
unset TUR_RUN_TEST_PRESET_UNSET
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'preset=host-debug build_dir=build/host-debug' <<< "$OUT"; then
  pass "nested: env_var_or_default + if + / concat compose"
else
  fail "nested: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 6. Env var override wins over the default.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
p := env_var_or_default("TUR_RUN_TEST_OVERRIDE", "default-p")

show:
    @echo "p={{p}}"
EOF
TUR_RUN_TEST_OVERRIDE="from-env" OUT="$(TUR_RUN_TEST_OVERRIDE=from-env "$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'p=from-env' <<< "$OUT"; then
  pass "env override wins over default"
else
  fail "env override: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 7. Forward-reference to a not-yet-defined variable is a clear error.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
a := b
b := "x"

show:
    @echo "{{a}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -ne 0 ] && grep -q "unknown variable 'b'" <<< "$OUT"; then
  pass "forward-ref: clear error"
else
  fail "forward-ref: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 8. Bare identifier RHS: a plain word RHS resolves to the previously-
#    defined variable (regression guard for the balanced scanner not
#    eating identifiers).
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
a := "hello"
b := a

show:
    @echo "b={{b}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'b=hello' <<< "$OUT"; then
  pass "bare-ident RHS resolves to prior var"
else
  fail "bare-ident RHS: got rc=$RC out=$OUT"
fi

exit $FAIL
