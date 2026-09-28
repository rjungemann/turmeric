#!/usr/bin/env bash
# tests/check-r7rs-srfi-prune.sh -- r7rs-srfi-plan S3: an SRFI costs a
# program only what the program reaches.
#
# `(import (srfi N))` splices the SRFI's definitions into the program once
# per compile (D3), and src/passes/srfi_prune.c drops each one nothing
# outside the SRFI files reaches before emission.  This compares `tur emit-c`
# for three programs:
#
#   p0  `(write 1)`, no SRFI;
#   p1  p0 importing (srfi 1) and calling none of it;
#   p2  p1 calling `fold`.
#
# and checks that:
#
#   - p1's C is p0's, up to the numbering of lifted lambdas and temporaries
#     (an unused import costs nothing);
#   - p2's C defines `fold` and the helpers it calls, and none of SRFI 1's
#     other procedures (spot-checked: lset-xor, delete, filter, partition);
#   - with the pass off (TUR_NO_SRFI_PRUNE=1), p1 does carry SRFI 1, so the
#     first check is the pass's doing and not an empty splice;
#   - p2 builds and prints the right answer.
#
# tests/run-r7rs-import.sh covers the cross-module cases (an SRFI a library
# uses and the program does not, procedures used as values).
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-srfi-prune: $TUR not built" >&2
    exit 2
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-srfi-prune: $*"; FAILED=1; }

# One file name for all three, in three directories: the program's module is
# named after its file, and that name is in the C.
mkdir -p "$TMP/p0" "$TMP/p1" "$TMP/p2"
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(write 1)\n' > "$TMP/p0/prog.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 1))\n(write 1)\n' > "$TMP/p1/prog.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 1))\n(write (fold + 0 (list 1 2 3)))\n' > "$TMP/p2/prog.scm"

for p in p0 p1 p2; do
    if ! (cd "$TMP/$p" && "$TUR" emit-c prog.scm > prog.c 2> emit.err); then
        fail "$p: tur emit-c failed: $(grep -vE 'W006[01]' "$TMP/$p/emit.err" | head -3)"
    fi
done
(cd "$TMP/p1" && TUR_NO_SRFI_PRUNE=1 "$TUR" emit-c prog.scm > unpruned.c 2>/dev/null)

# Lifted lambdas, envs and temporaries are numbered in elaboration order, and
# the splice's own definitions shift the numbers; the digits are not the code.
norm() { sed -E 's/[0-9]+/N/g' "$1"; }
if ! diff <(norm "$TMP/p0/prog.c") <(norm "$TMP/p1/prog.c") > "$TMP/p0p1.diff"; then
    fail "an unused (import (srfi 1)) changes the C beyond numbering ($(grep -c '^[<>]' "$TMP/p0p1.diff") lines differ):"
    head -10 "$TMP/p0p1.diff"
fi
if ! grep -q 'srfi1_' "$TMP/p1/unpruned.c"; then
    fail "with the pass off, p1's C holds no SRFI 1 definition -- the check above proves nothing"
fi
# The C spelling of srfi1--NAME: `-` is `_hy`, so srfi1--fold is srfi1_hy_hyfold.
if ! grep -q 'srfi1_hy_hyfold' "$TMP/p2/prog.c"; then
    fail "p2 calls fold, but its C does not define it"
fi
for name in lset_hyxor delete filter partition; do
    if grep -q "srfi1_hy_hy$name\b" "$TMP/p2/prog.c"; then
        fail "p2 calls only fold, but its C still defines SRFI 1's $name"
    fi
done
out="$(cd "$TMP/p2" && "$TUR" build prog.scm -o prog 2>/dev/null && ./prog)"
if [ "$out" != "6" ]; then
    fail "p2 printed '$out', expected '6'"
fi

if [ $FAILED -ne 0 ]; then
    exit 1
fi
echo "PASS check-r7rs-srfi-prune: an unused (srfi 1) costs nothing; fold costs fold"
