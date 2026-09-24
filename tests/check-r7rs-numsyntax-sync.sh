#!/usr/bin/env bash
# tests/check-r7rs-numsyntax-sync.sh -- r7rs-lang-plan T0: the R7RS number
# parser exists twice.  src/compiler/r7rs_numsyntax.inc is the source (the
# source reader and the interpreter #include it); the file-scope C block of
# stdlib/r7rs/numsyntax.tur is its copy for the compiled back end, written by
# tools/gen-r7rs-numsyntax.py.  They must be the same text, or `1/2` reads one
# way in a source file and another through `read` or `string->number`.
set -uo pipefail
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-r7rs-numsyntax-sync: python3 not found"
    exit 0
fi
python3 - <<'PY'
import sys
tur = open("stdlib/r7rs/numsyntax.tur", encoding="ascii").read()
inc = open("src/compiler/r7rs_numsyntax.inc", encoding="ascii").read()
start = tur.index("```c\n") + 5
end = tur.index("```\n", start)
if tur[start:end] != inc:
    print("FAIL check-r7rs-numsyntax-sync: stdlib/r7rs/numsyntax.tur's C block "
          "differs from src/compiler/r7rs_numsyntax.inc -- regenerate with "
          "python3 tools/gen-r7rs-numsyntax.py")
    sys.exit(1)
print("PASS check-r7rs-numsyntax-sync (%d bytes of C, identical)" % len(inc))
PY
