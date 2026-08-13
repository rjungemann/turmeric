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

pass=0
skip=0
fail=0
fail_list=""

for d in tests/fixtures/*/; do
  input="${d}input.tur"
  [ -f "$input" ] || input="${d}$(basename "$d").tur"
  [ -f "$input" ] || continue

  out="$("$TUR" audit-spans "$input" 2>/dev/null)"
  rc=$?
  case $rc in
    0) pass=$((pass + 1)) ;;
    3) fail=$((fail + 1))
       fail_list="${fail_list}FAIL span-coverage: ${input}
$(printf '%s\n' "$out" | sed 's/^/  /')
" ;;
    *) skip=$((skip + 1)) ;;  # 1 = no-elaborate, 2 = file error, anything else
  esac
done

if [ "$fail" -ne 0 ]; then
  printf '%s' "$fail_list"
  echo "FAIL span-coverage: $fail fixture(s) with breakpoint-span holes (pass=$pass skip=$skip)"
  exit 1
fi

echo "PASS span-coverage: $pass clean, $skip skipped (did not elaborate)"
