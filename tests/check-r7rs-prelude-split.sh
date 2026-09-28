#!/usr/bin/env bash
# tests/check-r7rs-prelude-split.sh -- r7rs-programs-compile-slowly: a
# `#lang r7rs` program built as two units (src/compiler/emit_split.h) keeps
# ONE copy of every piece of state.
#
# The library unit (the runtime preamble and the auto-loaded stdlib) is
# compiled once and cached; the program unit declares what it uses from it.
# A variable both units define is two variables: a parameter object, a
# handler stack or the collector's heap would silently fork, and the program
# would run on one copy while the prelude runs on the other.  The emitter
# decides unit by unit what to define and what to declare, and the state
# transform (emit_split_state) rewrites the text it writes verbatim.  This is
# the mechanical check on both: for each program below,
#
#   - no data symbol is DEFINED in both objects (`nm`: B/b D/d G/g S/s),
#     except the per-unit `any` name tables each unit registers for itself;
#   - two different programs link the SAME cached library object, so the
#     cache is doing its job;
#   - each program prints its fixture's expected output.
#
# Weak symbols (V/v) are the keyword records both units share by design
# (SYM2); read-only data (R/r) may be duplicated freely.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-prelude-split: $TUR not built" >&2
    exit 2
fi
CC_BIN="${CC:-cc}"
case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64) ;;
    *) echo "SKIP check-r7rs-prelude-split: 64-bit targets only"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-prelude-split: $*"; FAILED=1; }

# Per-unit by design: each unit registers its own rows of `any` type names.
ALLOW='^(__tur_any_chunk|__tur_any_rows)$'

defined_data() {
    nm "$1" 2>/dev/null | awk '$2 ~ /^[BbDdGgSs]$/ { n = $3; sub(/\.[0-9]+$/, "", n); print n }' | sort -u
}

FIXTURES="r7rs-named-let-sum r7rs-strings r7rs-ports r7rs-procedure-identity r7rs-colon-identifiers r7rs-gc-seam r7rs-type-errors-raise"
# The library object each of the first two programs linked (bash 3.2 has no
# associative arrays, and macOS ships it).
LIB_A=""
LIB_B=""
for f in $FIXTURES; do
    src="tests/fixtures/$f/input.tur"
    [ -f "$src" ] || { fail "$f: no $src"; continue; }
    d="$TMP/$f"
    mkdir -p "$d"
    # A private copy: the program unit's C lands at a path derived from the
    # input's, which must not be one a concurrent suite run also writes.
    cp -r "tests/fixtures/$f/." "$d/"
    log="$d/build.log"
    if ! (cd "$d" && TUR_PRELUDE_SPLIT=1 TUR_SHOW_CC=1 "$TUR" build input.tur -o "$d/prog") \
            >"$log" 2>&1; then
        fail "$f: build failed"; sed 's/^/    /' "$log" | tail -20; continue
    fi
    if grep -q "prelude split declined\|prelude split build failed" "$log"; then
        fail "$f: the split was not used"; grep "split" "$log" | sed 's/^/    /'; continue
    fi
    link=$(grep '^CC: ' "$log" | grep -v ' -c -o ' | tail -1)
    lib=$(printf '%s\n' "$link" | grep -o '[^ ]*/prelude/[0-9a-f]*\.o' | head -1)
    cli=$(printf '%s\n' "$link" | sed 's/.* -o [^ ]* \([^ ]*\.c\) .*/\1/')
    if [ -z "$lib" ] || [ ! -f "$lib" ] || [ ! -f "$cli" ]; then
        fail "$f: could not find the two units in the link line"
        printf '    %s\n' "$link"; continue
    fi
    case "$f" in
        r7rs-named-let-sum) LIB_A="$lib" ;;
        r7rs-strings)       LIB_B="$lib" ;;
    esac
    if ! "$CC_BIN" -O2 -std=c99 -w -fno-strict-aliasing -Isrc/runtime -c -o "$d/client.o" "$cli" \
            >"$d/cc.log" 2>&1; then
        fail "$f: the program unit does not compile on its own"; tail -5 "$d/cc.log"; continue
    fi
    both=$(comm -12 <(defined_data "$lib") <(defined_data "$d/client.o") | grep -Ev "$ALLOW")
    if [ -n "$both" ]; then
        fail "$f: state defined in both units:"
        printf '%s\n' "$both" | sed 's/^/    /' | head -20
    fi
    exp="tests/fixtures/$f/expected.stdout"
    if [ -f "$exp" ]; then
        if ! (cd "$d" && "$d/prog" >"$d/out" 2>/dev/null; true) || ! diff -q "$d/out" "$exp" >/dev/null; then
            fail "$f: output differs from $exp"
            diff "$d/out" "$exp" | head -10 | sed 's/^/    /'
        fi
    fi
done

if [ -n "$LIB_A" ] && [ -n "$LIB_B" ] && [ "$LIB_A" != "$LIB_B" ]; then
    fail "r7rs-named-let-sum and r7rs-strings built different library units" \
         "(${LIB_A##*/} vs ${LIB_B##*/}): the cache never hits"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "PASS check-r7rs-prelude-split"
fi
exit "$FAILED"
