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
# Read-only data (R/r) may be duplicated freely.  The keyword records are
# read-only too, but they must NOT be weak: PE/COFF has no weak data that
# folds, so the library unit defines each record and the program unit
# declares it (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md).
# A weak `__tur_sym_` symbol (V/v) in either unit fails.
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
# The split is on by default on Linux and Darwin (prelude_split_applies,
# src/main.c); Windows keeps one unit until its report closes
# (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md).
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP check-r7rs-prelude-split: the split is off by default on $(uname -s)"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-prelude-split: $*"; FAILED=1; }

# Per-unit by design: each unit registers its own rows of `any` type names
# (Mach-O spells a C name with a leading `_`).
ALLOW='^_?(__tur_any_chunk|__tur_any_rows)$'

# WRITABLE data only -- read-only data may be duplicated freely, and the two
# platforms need different questions asked to tell the two apart.
#
# ELF: `nm`'s one-letter class already separates them, R/r being rodata.
#
# Mach-O: it does NOT.  Plain `nm` spells every non-text local `s` and every
# non-text external `S`, whatever section it sits in, so a `static const` table
# is indistinguishable from mutable state.  The collector pasted into both units
# has one (`tur_gc_class_size`), which made this check fail on Darwin the first
# time it ran there with nothing actually wrong.  `nm -m` names the section
# instead, so ask for the writable ones by name: __data / __bss / __common, plus
# the two a `__thread` variable produces (__thread_vars holds its descriptor,
# __thread_bss its initial image).  __TEXT,__const, __TEXT,__cstring and
# __DATA,__const are not state: the last one sits outside __TEXT only because it
# needs relocating on load, and is read-only thereafter.
#
# The ELF arm has the same blind spot one section over: `.data.rel.ro` is
# read-only after load but `nm` classes it `d`, so a `static const` table of
# function pointers reads as writable state there.  Nothing in the collector has
# one today -- r7gc.c's allocator table is a local for exactly this reason --
# and if that changes, this arm needs `nm --format=sysv` and a section filter
# too, not another name on ALLOW.
defined_data() {
    if [ "$(uname -s)" = Darwin ]; then
        # The `^_` keeps assembler-local labels (`ltmp1`, `l_.str`) out: a C
        # name in a Mach-O object always carries the leading underscore.
        nm -m "$1" 2>/dev/null |
            awk '/\(__DATA,__(data|bss|common|thread_vars|thread_bss)\)/ {
                     n = $NF; sub(/\.[0-9]+$/, "", n); if (n ~ /^_/) print n }' |
            sort -u
    else
        nm "$1" 2>/dev/null |
            awk '$2 ~ /^[BbDdGgSs]$/ && $3 ~ /./ { n = $3; sub(/\.[0-9]+$/, "", n); print n }' |
            sort -u
    fi
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
    weak=$(nm "$lib" "$d/client.o" 2>/dev/null | awk '$2 ~ /^[Vv]$/ && $3 ~ /__tur_sym_/ { print $3 }' | sort -u)
    if [ -n "$weak" ]; then
        fail "$f: weak keyword records (they do not fold on Windows):"
        printf '%s\n' "$weak" | sed 's/^/    /' | head -5
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
