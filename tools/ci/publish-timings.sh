#!/usr/bin/env bash
#
# Append suite timing rows to the `ci-metrics` orphan branch.
#
# Phase 3A of docs/upcoming/suite-timing-trends-plan.md.  The branch carries no
# source -- just one JSONL file per year -- so it never builds and never merges.
# CI does not run on it: .github/workflows/ci.yml triggers only on `main`, and
# the commit message additionally carries [skip ci].
#
# Usage: publish-timings.sh timings.jsonl
#
# Deliberate deviation from the plan: it specified a force-push.  A force-push
# can silently discard rows another run appended between our fetch and our push,
# which is the one outcome an append-only metrics log must never have.  The
# concurrency group in the workflow already serializes publishers, so a plain
# push should succeed; on the rare rejection we re-fetch, re-append onto the new
# tip, and retry.  Nothing is ever overwritten.
set -euo pipefail

BRANCH="${TIMINGS_BRANCH:-ci-metrics}"
INPUT="${1:?usage: publish-timings.sh <timings.jsonl>}"
ATTEMPTS="${TIMINGS_PUSH_ATTEMPTS:-5}"

if [ ! -s "$INPUT" ]; then
    echo "publish-timings: $INPUT is missing or empty; nothing to publish" >&2
    exit 0
fi

INPUT_ABS="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
YEAR="$(date -u +%Y)"
FILE="suite-timings-${YEAR}.jsonl"
ROWS="$(wc -l < "$INPUT_ABS" | tr -d ' ')"

if [ -z "$(git config user.email || true)" ]; then
    git config user.email "github-actions[bot]@users.noreply.github.com"
    git config user.name  "github-actions[bot]"
fi

WT="$(mktemp -d)"
cleanup() {
    git worktree remove --force "$WT" >/dev/null 2>&1 || true
    rm -rf "$WT"
}
trap cleanup EXIT

attempt=1
while [ "$attempt" -le "$ATTEMPTS" ]; do
    rm -rf "$WT"

    if git ls-remote --exit-code --heads origin "$BRANCH" >/dev/null 2>&1; then
        # Existing branch: check it out into a throwaway worktree. Full history
        # (no --depth) so the push is a fast-forward rather than a shallow-push
        # rejection.
        git fetch --force origin "$BRANCH:refs/remotes/origin/$BRANCH"
        git worktree add -f -B "$BRANCH" "$WT" "origin/$BRANCH" >/dev/null
    else
        # First ever run: create the orphan with no parent and no source files.
        git worktree add -f --detach "$WT" HEAD >/dev/null
        (
            cd "$WT"
            git checkout --orphan "$BRANCH" >/dev/null 2>&1
            git rm -rf --cached . >/dev/null 2>&1 || true
            find . -mindepth 1 -maxdepth 1 -not -name '.git' -exec rm -rf {} +
            cat > README.md <<'EOF'
# ci-metrics

Data-only branch. No source, no build, no CI.

`suite-timings-<year>.jsonl` holds one JSON object per CTest suite per CI run,
appended by `tools/ci/publish-timings.sh` on pushes to `main`. See
`docs/upcoming/suite-timing-trends-plan.md` on `main` for the schema and the
reason timings are only comparable within a fixed
(build_type, os, cc, nproc, jit) tuple.

Never merge this branch into `main`.
EOF
        )
    fi

    cat "$INPUT_ABS" >> "$WT/$FILE"

    (
        cd "$WT"
        git add "$FILE" README.md 2>/dev/null || git add "$FILE"
        if git diff --cached --quiet; then
            echo "publish-timings: no change to commit" >&2
            exit 0
        fi
        # [skip ci] is the second layer of the "this branch never builds"
        # guarantee; the workflow's own `branches: [main]` filter is the first.
        git commit -q -m "ci-metrics: ${ROWS} suite rows from ${GITHUB_SHA:-local} [skip ci]"
    )

    if git -C "$WT" push origin "$BRANCH"; then
        echo "publish-timings: appended ${ROWS} rows to ${BRANCH}/${FILE}" >&2
        exit 0
    fi

    echo "publish-timings: push rejected (attempt ${attempt}/${ATTEMPTS}); re-fetching and retrying" >&2
    attempt=$((attempt + 1))
    sleep $(( attempt * 2 ))
done

echo "publish-timings: giving up after ${ATTEMPTS} attempts" >&2
exit 1
