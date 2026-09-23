#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$1"
cp "$FIXTURE_DIR/program.tur" "$TMP/saffron.tur"
sed 's/^#lang saffron$/#lang r7rs/' "$FIXTURE_DIR/program.tur" > "$TMP/r7rs.tur"
# R1 pinned byte-identical emitted C for the two directives.  From R2 the r7rs
# prelude (stdlib/r7rs/prelude.tur) differs from Saffron's, so the C differs
# by exactly those definitions; what stays pinned is the thesis itself -- the
# same program, under both directives, on both back ends, prints the same.
"$TUR" run "$TMP/r7rs.tur" 2>/dev/null > "$TMP/compiled.out"
"$TUR" --interpret "$TMP/r7rs.tur" 2>/dev/null > "$TMP/interp.out"
"$TUR" run "$TMP/saffron.tur" 2>/dev/null > "$TMP/saffron.out"
cmp -s "$TMP/compiled.out" "$TMP/saffron.out" && echo "compiled stdout: identical to saffron"
cmp -s "$TMP/interp.out"   "$TMP/saffron.out" && echo "interpreted stdout: identical to saffron"
cat "$TMP/compiled.out"
