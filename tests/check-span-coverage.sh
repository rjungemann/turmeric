#!/usr/bin/env bash
#
# check-span-coverage.sh -- debugger Phase 1 span-coverage gate.
#
# Walks every fixture under tests/fixtures/ and runs `tur audit-spans` on its
# entry source.  A fixture FAILS the gate when the elaborated AST contains a
# breakpoint-eligible node (a top-level form, defn, let form, or call site)
# with no usable source span -- exactly the holes a debugger would choke on
# when resolving a breakpoint or stack frame to a source location.
#
# `tur audit-spans` exit codes:
#   0  clean       -- audited, every breakpoint-eligible node has a span
#   3  holes       -- audited, one or more nodes lack a usable span  -> FAIL
#   1  no-elaborate -- the file did not elaborate (error fixture, or a
#                      requires.spices fixture whose sibling repo is absent) -> SKIP
#   2  file error  -- input unreadable                                       -> SKIP
#
# The audits run in parallel across the machine's cores.  Each one elaborates
# the whole stdlib under a sanitized Debug `tur` (~0.15 s), and there are
# ~2300 fixtures, so the serial loop this used to be was the single longest
# auxiliary suite in CI by a wide margin: 5.5 min on the 3-core macOS runner,
# 3.8 min on Linux, per the /ci timings.  Every audit is independent, so
# `xargs -P` is the whole change; each audit writes one result file (verdict,
# input path, then the audit output) and the tally reads them back in path
# order so the report is stable.  Set TUR_SPAN_JOBS to override the worker
# count.
#
# This is the gate for the Phase 1 exit criterion in
# docs/archive/history/debugger-plan.md.  See docs/artifacts/debugger-spans-audit.md
# for the audit write-up.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL span-coverage: tur binary not found at $TUR (build first)"
  exit 1
fi
case "$TUR" in
  /*) ;;
  *)  TUR="$(pwd)/$TUR" ;;
esac
export TUR

JOBS="${TUR_SPAN_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
RES="$(mktemp -d -t tur-span.XXXXXX)"
trap 'rm -rf "$RES"' EXIT
export RES

# audit_one <input> -- one result file: "<verdict>\n<input>\n<audit output>".
# The file is named by a checksum of the path (cksum is POSIX; md5sum is not
# on macOS), and the path is stored inside rather than reconstructed from the
# name, so fixture names with any character in them round-trip.
audit_one() {
  local input="$1" out rc key
  out="$("$TUR" audit-spans "$input" 2>/dev/null)"
  rc=$?
  key="$(printf '%s' "$input" | cksum | cut -d' ' -f1)"
  case $rc in
    0) printf 'pass\n%s\n' "$input" > "$RES/$key" ;;
    3) { printf 'fail\n%s\n' "$input"; printf '%s\n' "$out"; } > "$RES/$key" ;;
    *) printf 'skip\n%s\n' "$input" > "$RES/$key" ;;  # 1 = no-elaborate, 2 = file error, anything else
  esac
}
export -f audit_one

for d in tests/fixtures/*/; do
  input="${d}input.tur"
  [ -f "$input" ] || input="${d}$(basename "$d").tur"
  [ -f "$input" ] || continue
  printf '%s\n' "$input"
done | xargs -P "$JOBS" -I{} bash -c 'audit_one "$1"' _ {}

pass=0
skip=0
fail=0
fail_list=""

# Tally in input-path order: line 2 of each result file is the path.
while IFS= read -r f; do
  verdict="$(sed -n '1p' "$f")"
  input="$(sed -n '2p' "$f")"
  case "$verdict" in
    pass) pass=$((pass + 1)) ;;
    skip) skip=$((skip + 1)) ;;
    fail) fail=$((fail + 1))
          fail_list="${fail_list}FAIL span-coverage: ${input}
$(tail -n +3 "$f" | sed 's/^/  /')
" ;;
  esac
done < <(for f in "$RES"/*; do printf '%s\t%s\n' "$(sed -n '2p' "$f")" "$f"; done | sort | cut -f2)

if [ "$fail" -ne 0 ]; then
  printf '%s' "$fail_list"
  echo "FAIL span-coverage: $fail fixture(s) with breakpoint-span holes (pass=$pass skip=$skip)"
  exit 1
fi

echo "PASS span-coverage: $pass clean, $skip skipped (did not elaborate)"
