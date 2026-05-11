#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if rg -n "SPAN_UNKNOWN" tests/fixtures --glob '**/expected.c' > /tmp/span_unknown_hits.txt; then
  echo "FAIL span-unknown-guard: SPAN_UNKNOWN found in expected.c snapshots"
  cat /tmp/span_unknown_hits.txt
  exit 1
fi

echo "PASS span-unknown-guard"
