#!/usr/bin/env bash
# Lint: no `WRITER | grep -q ...` in a test script that sets `pipefail`.
#
# `grep -q` exits the instant it finds its first match.  If the writer on the
# left of the pipe is still producing output it takes SIGPIPE and dies with
# 141, and under `set -o pipefail` that 141 becomes the PIPELINE's status --
# so the pipeline reports failure precisely BECAUSE the pattern was found.
# `if ! cmd | grep -q PATTERN` then takes the "not found" branch when the
# pattern is there.  Whether it fires depends on whether the writer finishes
# before grep exits, so it scales with payload size and machine load: green by
# hand, red in CI, green again on the re-run.  This has bitten the repo three
# times; see docs/archive/pipefail-grep-q-false-failures.md.
#
# Immune spellings, in order of preference:
#   1. [[ "$var" == *"needle"* ]]     -- pure bash, no subprocess, no pipe
#   2. grep -q needle <<< "$var"      -- here-string, nothing to SIGPIPE
#   3. grep -q needle < <(cmd)        -- writer is not in the pipeline
# Do NOT "fix" a site by dropping pipefail; that hides real writer failures.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

violations=0
while IFS= read -r f; do
    [ "$f" = "tests/check-pipefail-grep-q.sh" ] && continue
    grep -q 'pipefail' "$f" || continue
    # Strip whole-line comments before matching so the notes above (and the
    # ones in the swept scripts) do not trip their own lint.
    hits=$(grep -nE '(^|[^|])\|[ ]*grep([ ]+-[A-Za-z-]+)*[ ]+-[A-Za-z-]*q' "$f" \
             | grep -v '^[0-9]*: *#' || true)
    [ -n "$hits" ] || continue
    while IFS= read -r hit; do
        echo "FAIL check-pipefail-grep-q: $f:$hit"
        violations=$((violations + 1))
    done <<< "$hits"
done < <(find tests -name '*.sh' -type f | sort)

if [ "$violations" -ne 0 ]; then
    echo ""
    echo "$violations pipe-into-grep-q site(s) in pipefail scripts."
    echo 'Rewrite as `grep -q PATTERN <<< "$var"` (or a bash substring match);'
    echo "see the header of $0."
    echo "check-pipefail-grep-q summary: $violations violation(s)"
    exit 1
fi

echo 'PASS check-pipefail-grep-q (no pipe-into-grep-q under pipefail)'
echo "check-pipefail-grep-q summary: 0 violations"
