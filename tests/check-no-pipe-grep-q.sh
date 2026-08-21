#!/usr/bin/env bash
# tests/check-no-pipe-grep-q.sh -- reject `... | grep -q` in a script that sets
# `pipefail`.
#
# Why this is a lint and not a style nit: `grep -q` exits the instant it finds
# its first match, which SIGPIPEs whatever is still writing on the left of the
# pipe. That writer dies with 141, and under `set -o pipefail` the pipeline's
# status IS that 141 -- so the pipeline reports FAILURE precisely because the
# pattern was found. An `if ! ... | grep -q PAT` then takes the "not found"
# branch when PAT is present.
#
# Whether it fires depends on whether the writer finishes before grep exits, so
# it scales with payload size and machine load: green by hand, red in CI, green
# again on a re-run. It has bitten this repo at least three times
# (rp6-watch-with-help in 5600eff2; check-export-from-no-wrapper and
# run-build-project on 2026-08-20), each time reading as a product bug rather
# than a harness bug -- which is what makes it worth a lint.
#
# Immune spellings, in order of preference:
#   1. grep -q PAT <<< "$var"        -- here-string; nothing to SIGPIPE
#   2. [[ "$var" == *"needle"* ]]    -- pure bash, no subprocess
#   3. grep -q PAT file              -- no pipe at all
#
# Do NOT "fix" an instance by dropping pipefail; that hides real failures in
# the writer.
set -uo pipefail
cd "$(dirname "$0")/.."

FAIL=0
found=""
SELF="tests/check-no-pipe-grep-q.sh"
while IFS= read -r f; do
    [ "$f" = "$SELF" ] && continue          # this file quotes the pattern it bans
    grep -q "pipefail" "$f" 2>/dev/null || continue
    # A REAL pipe into grep -q. `(^|[^|])` excludes `|| grep -q PAT file`, which
    # is a logical-or onto a file grep -- no pipe, nothing to SIGPIPE.
    # Comment-only lines are stripped so the immune spellings can be described.
    hits=$(grep -nE '(^|[^|])\| *grep -q' "$f" 2>/dev/null | grep -v '^[0-9]*: *#' || true)
    if [ -n "$hits" ]; then
        found="$found$f\n$(printf '%s\n' "$hits" | sed 's/^/    /')\n"
        FAIL=1
    fi
done <<EOF
$(find tests tools -name '*.sh' | sort)
EOF

if [ "$FAIL" -ne 0 ]; then
    echo "FAIL no-pipe-grep-q: \`| grep -q\` under pipefail (see the header of $0):"
    printf "%b" "$found"
    exit 1
fi

echo "PASS no-pipe-grep-q"
exit 0
