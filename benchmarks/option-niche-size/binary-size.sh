#!/usr/bin/env bash
# SR3 item 4, the code-size dimension: what the niche costs or saves in the
# EMITTED ARTIFACT, as opposed to per value.
#
# The 16 -> 8 claim is about a value's storage.  A representation change also
# moves code: the default path emits a monomorph typedef, two constructors
# that zero a tag and write a union arm, and a tag test at every match; the
# niche emits an identity constructor, a NULL constructor, and a pointer test.
# Neither direction is obvious in advance, which is why it is measured here
# rather than asserted.
#
# Measures the emitted translation unit as an OBJECT (-c), not a linked
# binary: the object is exactly the emitted code, with no libturi to cancel
# out and no sanitizer runtime to inflate it.
#
# Usage: bash benchmarks/option-niche-size/binary-size.sh <input.tur>...
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
CC="${CC:-cc}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

printf '%-44s %10s %10s %9s  %s\n' fixture default niche delta unit

for input in "$@"; do
    name="$(basename "$(dirname "$input")")"

    # The niche is the default since 2026-09-03; the tagged side is the
    # TUR_OPTION_NICHE=0 bisection hatch.  Column names kept (default = tagged).
    TUR_OPTION_NICHE=0 "$TUR" emit-c "$input" > "$W/d.c" 2>/dev/null || { echo "$name: emit-c failed (tagged, TUR_OPTION_NICHE=0)"; continue; }
    "$TUR" emit-c "$input" > "$W/n.c" 2>/dev/null || { echo "$name: emit-c failed (niche, default)"; continue; }

    # Compile to an OBJECT, not a linked binary.  Linking would pull in
    # libturi, which is identical between the two runs and so cancels in the
    # delta -- but the Debug libturi is built `-fsanitize=address,undefined`,
    # so an unsanitized fixture compile does not link against it at all
    # (undefined `__asan_report_load8`), and a sanitized one inflates .text
    # by an order of magnitude.  The object isolates exactly the emitted
    # code, which is the thing whose size the representation moves.
    ok=1
    for v in d n; do
        "$CC" -O2 -std=c99 -w -fno-strict-aliasing -c "$W/$v.c" -o "$W/$v.o" \
            2>/dev/null || ok=0
    done
    [ "$ok" = 1 ] || { echo "$name: cc failed"; continue; }

    dt=$(size "$W/d.o" | awk 'NR==2 {print $1}')
    nt=$(size "$W/n.o" | awk 'NR==2 {print $1}')
    printf '%-44s %10s %10s %+9d  bytes .text\n' "$name" "$dt" "$nt" "$((nt - dt))"
done
