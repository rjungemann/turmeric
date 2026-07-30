#!/usr/bin/env bash
# run-tur-version.sh -- `:tur-version` in build.tur.
#
# A manifest could not say which compiler versions a spice is valid under, so
# every version-skew failure surfaced as an error about the wrong thing: a caret
# under correct, current source with no mention of the compiler version. See
# docs/reported/no-compiler-version-constraint-in-manifest.md.
#
# What is asserted here, and why each case matters:
#
#   - A satisfied range is SILENT. The key must cost nothing when it is met,
#     which is the case for every existing manifest (none declares one).
#   - An unsatisfied FLOOR is a hard error AND a non-zero exit. The exit status
#     is the whole point: the check first printed an "error" and still exited 0,
#     because every compile entry point calls diag_reset() to keep batch drivers
#     from poisoning later files, which wiped the manifest-time error. An error
#     that does not fail is not an error. (The pre-existing TUR-E0620 manifest
#     error still has that shape.)
#   - An unsatisfied CEILING warns and exits 0. A spice untested against a newer
#     compiler usually still works; a hard ceiling would mean every compiler
#     release breaks every spice until each author bumps a number.
#   - A MALFORMED range is a hard error, so a typo cannot silently become a
#     different constraint.
#
# The floor/ceiling cases are written against a version far from any real one so
# the test does not need updating every release.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/src"
cat > "$TMP/src/main.tur" <<'EOF'
(defn main [] : int 0)
EOF

# Write a manifest with the given :tur-version and check the outcome.
#   $1 label, $2 range, $3 expected exit, $4 expected diagnostic code ("" = none)
check() {
    local label="$1" range="$2" want_rc="$3" want_code="$4"
    cat > "$TMP/build.tur" <<EOF
(defpackage tvcheck
  :version "0.1.0"
  :tur-version "$range"
  :modules [main])
EOF
    local out rc
    out="$("$TUR" check "$TMP/src/main.tur" 2>&1)"
    rc=$?

    if [ "$rc" -ne "$want_rc" ]; then
        fail "$label -- expected exit $want_rc, got $rc"
        return
    fi
    if [ -n "$want_code" ] && ! grep -q "$want_code" <<<"$out"; then
        fail "$label -- expected $want_code in output; got: $(head -1 <<<"$out")"
        return
    fi
    if [ -z "$want_code" ] && grep -qE 'TUR-(E0621|E0622|W0623)' <<<"$out"; then
        fail "$label -- expected no :tur-version diagnostic; got: $(head -1 <<<"$out")"
        return
    fi
    pass "$label"
}

CUR="$("$TUR" --version | head -1 | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | tr -d 'v')"
echo "-- running tur $CUR --"

# Satisfied: silent, exit 0.
check "floor met is silent"            ">=0.0.1"              0 ""
check "range containing current"       ">=0.0.1, <99.0.0"     0 ""
check "exact match is silent"          ">=$CUR"               0 ""

# Floor missed: hard error, non-zero exit.
check "floor missed errors"            ">=99.0.0"             1 "TUR-E0621"

# Ceiling exceeded: warning, exit 0.
check "ceiling exceeded warns only"    ">=0.0.1, <0.0.2"      0 "TUR-W0623"

# Malformed: hard error.
check "malformed range errors"         ">=junk"               1 "TUR-E0622"
check "bare comparator errors"         ">="                   1 "TUR-E0622"
check "unsupported tilde errors"       "~1.0"                 1 "TUR-E0622"

# No key at all: the overwhelmingly common case must be untouched.
cat > "$TMP/build.tur" <<'EOF'
(defpackage tvcheck
  :version "0.1.0"
  :modules [main])
EOF
OUT="$("$TUR" check "$TMP/src/main.tur" 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && ! grep -qE 'TUR-(E0621|E0622|W0623)' <<<"$OUT"; then
    pass "manifest without :tur-version is unaffected"
else
    fail "manifest without :tur-version is unaffected (rc=$RC)"
fi

# The caret's 0.x rule: ^0.X.Y admits later PATCH but not the next MINOR.
# Only meaningful while the compiler is pre-1.0.
MAJ="${CUR%%.*}"
if [ "$MAJ" = "0" ]; then
    MIN="$(cut -d. -f2 <<<"$CUR")"
    check "caret admits current 0.x"      "^0.$MIN.0"            0 ""
    check "caret excludes next minor"     "^0.$((MIN + 1)).0"    1 "TUR-E0621"
else
    echo "SKIP: caret 0.x checks -- compiler is $CUR, not pre-1.0"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "tur-version: FAILED"
    exit 1
fi
echo "tur-version: all checks passed"
