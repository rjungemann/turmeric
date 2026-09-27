#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$1"
cp "$FIXTURE_DIR/program.tur" "$TMP/saffron.tur"
sed 's/^#lang saffron$/#lang r7rs/' "$FIXTURE_DIR/program.tur" > "$TMP/r7rs.tur"
# The program under Saffron: unchanged.
"$TUR" run "$TMP/saffron.tur" 2>/dev/null
# The same text under #lang r7rs: refused at its first Turmeric form
# (r7rs-turmeric-syntax-leaks item 7), compiled and interpreted.
refused() {
    if grep -q "'defn' is Turmeric syntax, not Scheme; write Turmeric code in a Turmeric module" "$1"; then
        echo "r7rs ($2): refused -- 'defn' is Turmeric syntax, not Scheme"
    else
        echo "r7rs ($2): NOT refused"
    fi
}
"$TUR" run "$TMP/r7rs.tur" > /dev/null 2> "$TMP/compiled.err" || true
refused "$TMP/compiled.err" compiled
ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP/r7rs.tur" > /dev/null 2> "$TMP/interp.err" || true
refused "$TMP/interp.err" interpreted
