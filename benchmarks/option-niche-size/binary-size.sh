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
# Usage: bash benchmarks/option-niche-size/binary-size.sh <input.tur>...
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
CC="${CC:-cc}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

printf '%-44s %10s %10s %9s  %s\n' fixture default niche delta unit

for input in "$@"; do
    name="$(basename "$(dirname "$input")")"

    "$TUR" emit-c "$input" > "$W/d.c" 2>/dev/null || { echo "$name: emit-c failed (default)"; continue; }
    "$TUR" --enable=option-niche emit-c "$input" > "$W/n.c" 2>/dev/null || { echo "$name: emit-c failed (niche)"; continue; }

    # -O2 and no sanitizer: this measures the shipped shape, not the Debug
    # compiler's.  Fixture programs are built exactly this way by run.sh.
    ok=1
    for v in d n; do
        "$CC" -O2 -std=c99 -w -fno-strict-aliasing "$W/$v.c" \
            -L"$ROOT/build/src" -lturi -lm -lpthread -o "$W/$v.bin" 2>/dev/null || ok=0
    done
    [ "$ok" = 1 ] || { echo "$name: cc failed"; continue; }

    # `size` reports the linked artifact, which includes all of libturi that
    # got pulled in -- identical between the two, so the DELTA is the emitted
    # code's and the absolute figure is not worth quoting.
    dt=$(size "$W/d.bin" | awk 'NR==2 {print $1}')
    nt=$(size "$W/n.bin" | awk 'NR==2 {print $1}')
    printf '%-44s %10s %10s %+9d  bytes .text\n' "$name" "$dt" "$nt" "$((nt - dt))"
done
