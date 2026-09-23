#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$1"
cp "$FIXTURE_DIR/program.tur" "$TMP/saffron.tur"
sed 's/^#lang saffron$/#lang r7rs/' "$FIXTURE_DIR/program.tur" > "$TMP/r7rs.tur"
# Emitted C embeds the source path (comments, #line); normalise the one
# token that differs between the two compiles.
"$TUR" emit-c "$TMP/saffron.tur" 2>/dev/null | sed 's/saffron\.tur/X.tur/g; s/saffron_tur/X_tur/g' > "$TMP/saffron.c"
"$TUR" emit-c "$TMP/r7rs.tur"    2>/dev/null | sed 's/r7rs\.tur/X.tur/g; s/r7rs_tur/X_tur/g'       > "$TMP/r7rs.c"
if cmp -s "$TMP/saffron.c" "$TMP/r7rs.c"; then
    echo "emit-c: identical"
else
    echo "emit-c: DIFFERS"
    diff "$TMP/saffron.c" "$TMP/r7rs.c" | head -20 >&2
    exit 1
fi
"$TUR" run "$TMP/r7rs.tur" 2>/dev/null > "$TMP/compiled.out"
"$TUR" --interpret "$TMP/r7rs.tur" 2>/dev/null > "$TMP/interp.out"
"$TUR" run "$TMP/saffron.tur" 2>/dev/null > "$TMP/saffron.out"
cmp -s "$TMP/compiled.out" "$TMP/saffron.out" && echo "compiled stdout: identical to saffron"
cmp -s "$TMP/interp.out"   "$TMP/saffron.out" && echo "interpreted stdout: identical to saffron"
cat "$TMP/compiled.out"
