#!/usr/bin/env bash
# run-tur-run-alias.sh -- regression for tur-run-alias-breaks-snapshot-ci-guard.
#
# `tur run` used to abort parsing the whole Justfile on an `alias NAME :=
# TARGET` line, so any recipe after one was unreachable -- which silently
# disabled the snapshot-drift CI guard. This test creates a tiny Justfile
# with an alias, ensures `tur run` does not abort the parse, and that the
# alias resolves to its target recipe.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

cat > Justfile <<'EOF'
alias hi := greet

greet:
    echo "hello from greet"

after-alias:
    echo "after-alias reached"
EOF

# 1. Alias resolves to its target.
OUT="$("$TUR" run hi 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q "hello from greet" <<< "$OUT"; then
  pass "alias resolves to target recipe"
else
  fail "alias resolves to target recipe (rc=$RC, out=$OUT)"
fi

# 2. Recipes after the alias are still reachable (the original regression).
OUT2="$("$TUR" run after-alias 2>&1)"
RC2=$?
if [ "$RC2" -eq 0 ] && grep -q "after-alias reached" <<< "$OUT2"; then
  pass "recipes after an alias remain reachable"
else
  fail "recipes after an alias remain reachable (rc=$RC2, out=$OUT2)"
fi

# 3. Original target name still works directly.
OUT3="$("$TUR" run greet 2>&1)"
RC3=$?
if [ "$RC3" -eq 0 ] && grep -q "hello from greet" <<< "$OUT3"; then
  pass "original target name still resolves directly"
else
  fail "original target name still resolves directly (rc=$RC3, out=$OUT3)"
fi

exit $FAIL
