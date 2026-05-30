#!/usr/bin/env bash
# run-tur-new.sh -- NW6 bootstrap acceptance gate.
#
# Verifies the whole `tur new` theme end-to-end: a freshly scaffolded spice
# must survive its own CI recipe without any special setup.
#
#   tur new tmp-spice
#   cd tmp-spice
#   tur run ci          # -> exit 0  (fans out to clean + check + test)
#
# The scaffolded `ci` recipe shells out to bare `tur ...` commands, so the
# built binary must be discoverable on PATH; we prepend its directory below.
# No `just`, network, or ../turmeric-spices checkout is required -- a bare
# library spice pulls only from builtin operators.
#
# See docs/tur-new-ci-bootstrap-plan.md.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
# The scaffolded Justfile recipes invoke bare `tur`; make it resolvable.
export PATH="$(cd "$(dirname "$TUR")" && pwd):$PATH"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

# 1. Scaffold a brand-new library spice (no git, to stay hermetic in CI).
if "$TUR" new tmp-spice --no-git >scaffold.log 2>&1; then
  pass "tur new tmp-spice"
else
  fail "tur new tmp-spice"
  cat scaffold.log
fi

if [ -d tmp-spice ]; then
  pass "scaffold directory created"
else
  fail "scaffold directory created"
  echo "Some tur new bootstrap tests failed."
  exit 1
fi

cd tmp-spice

# 2. The acceptance gate: the spice passes its own CI recipe end-to-end.
CI_OUT="$("$TUR" run ci 2>&1)"
CI_RC=$?
if [ "$CI_RC" -eq 0 ]; then
  pass "tur run ci (exit 0)"
else
  fail "tur run ci (exit $CI_RC)"
  echo "---- tur run ci output ----"
  echo "$CI_OUT"
  echo "---------------------------"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "All tur new bootstrap tests passed."
  exit 0
else
  echo "Some tur new bootstrap tests failed."
  exit 1
fi
