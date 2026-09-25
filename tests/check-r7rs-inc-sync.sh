#!/usr/bin/env bash
# tests/check-r7rs-inc-sync.sh -- r7rs-lang-plan T0/T1: the shared C of
# `#lang r7rs`'s numbers exists twice.  src/compiler/r7rs_bignum.inc and
# src/compiler/r7rs_numsyntax.inc are the sources (the source reader and the
# interpreter #include them); the file-scope C blocks of stdlib/r7rs/bignum.tur
# and stdlib/r7rs/numsyntax.tur are their copies for the compiled back end,
# written by tools/gen-r7rs-inc.py.  A copy that drifts makes the back ends
# compute or read a number differently, so each must be its .inc exactly.
set -uo pipefail
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-r7rs-inc-sync: python3 not found"
    exit 0
fi
python3 - <<'PY'
import sys
pairs = [("stdlib/r7rs/bignum.tur", "src/compiler/r7rs_bignum.inc"),
         ("stdlib/r7rs/numsyntax.tur", "src/compiler/r7rs_numsyntax.inc")]
bad = False
for tur_path, inc_path in pairs:
    tur = open(tur_path, encoding="ascii").read()
    inc = open(inc_path, encoding="ascii").read()
    start = tur.index("```c\n") + 5
    end = tur.index("```\n", start)
    if tur[start:end] != inc:
        print("FAIL check-r7rs-inc-sync: %s's C block differs from %s -- "
              "regenerate with python3 tools/gen-r7rs-inc.py" % (tur_path, inc_path))
        bad = True
    else:
        print("PASS check-r7rs-inc-sync: %s (%d bytes of C, identical)" % (tur_path, len(inc)))
sys.exit(1 if bad else 0)
PY
