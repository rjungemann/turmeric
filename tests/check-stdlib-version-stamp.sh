#!/usr/bin/env bash
# `stdlib/VERSION` must equal the top-level `VERSION`.
#
# The stamp is what lets `tur` tell "this stdlib is mine" from "this stdlib is
# some other release's".  Without it, pointing a compiler at a stdlib from a
# different vintage is accepted silently and fails much later as something like
# `no member named 'is_ok' in 'tur_result_box_t'` -- which names neither the
# stdlib nor TUR_STDLIB_DIR and reads as a codegen bug.  That cost a full
# investigation once; see docs/archive/type-fuzz-src-red-on-clang-21.md and
# docs/archive/stdlib-dir-guard-accepts-mismatched-stdlib.md.
#
# A stamp that drifts is worse than no stamp, because it is trusted: a stale
# `stdlib/VERSION` makes a genuinely mismatched stdlib look like a match, and
# makes the matching one warn.  The release commands bump both files together
# (.claude/commands/cut-*-release.md); this check is what makes forgetting loud
# at the next build rather than at the next mismatch.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() {
    echo "FAIL check-stdlib-version-stamp -- $1"
    echo ""
    echo "check-stdlib-version-stamp summary: 1 violation"
    exit 1
}

[ -f VERSION ]        || fail "no top-level VERSION file"
[ -f stdlib/VERSION ] || fail "stdlib/VERSION is missing. Create it with the \
same contents as the top-level VERSION; tur reads it to tell its own stdlib \
from another release's."

top=$(tr -d ' \t\r\n' < VERSION)
lib=$(tr -d ' \t\r\n' < stdlib/VERSION)

[ -n "$top" ] || fail "the top-level VERSION file is empty"
[ -n "$lib" ] || fail "stdlib/VERSION is empty, which tur reads as 'no stamp'"

if [ "$top" != "$lib" ]; then
    fail "stdlib/VERSION ($lib) != VERSION ($top). Bump both together -- a \
stale stamp makes a mismatched stdlib look like a match, and makes the correct \
one warn."
fi

echo "PASS check-stdlib-version-stamp (stdlib/VERSION == VERSION == $top)"
echo "check-stdlib-version-stamp summary: 0 violations"
