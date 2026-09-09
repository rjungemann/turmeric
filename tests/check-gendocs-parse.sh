#!/usr/bin/env bash
# check-gendocs-parse.sh -- tools/gendocs.py must parse a signature the same
# way in the fused (`[a :int] :int`) and spaced (`[a : int] : int`) spellings.
# Wraps tests/tools/check_gendocs_parse.py; skips cleanly without python3.
set -u
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-gendocs-parse -- python3 not available"
    exit 0
fi
exec python3 tests/tools/check_gendocs_parse.py
