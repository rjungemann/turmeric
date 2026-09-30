#!/usr/bin/env bash
# The gate withholds the acting: the program runs, and the compile prints
# nothing at all -- no experiment warning, no refinement diagnostic.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
err="$1/stderr.txt"
"$TUR" run "$FIXTURE_DIR/input.tur" 2> "$err"
if [ -s "$err" ]; then
    echo "expected no stderr with the gate off, got:" >&2
    cat "$err" >&2
    exit 1
fi
