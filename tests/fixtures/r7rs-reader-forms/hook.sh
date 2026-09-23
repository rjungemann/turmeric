#!/usr/bin/env bash
# r7rs-lang-plan R1: the Scheme reader's form-level output, pinned against
# the equivalent Turmeric spelling.  `tur parse-check` reads both files
# (honouring each one's `#lang` line), prints every top-level form with
# form_print, and exits 0 only when the two renderings are byte-identical --
# so a lexeme the Scheme reader mis-reads shows up as the differing line.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
"$TUR" parse-check "$FIXTURE_DIR/scheme.tur" "$FIXTURE_DIR/plain.tur"
echo "parse-check: identical"
