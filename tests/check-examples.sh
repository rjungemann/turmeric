#!/usr/bin/env bash
# tests/check-examples.sh -- `tur check` sweep over examples/, ratcheted.
#
# Nothing under tests/ or CMakeLists.txt referenced examples/ before this, so
# nothing compiled them and they rotted: at the time this was added, 9 of 17
# failed `tur check`, including four of the five datalog files the
# datalog-01..05 tutorial series walks through line by line.
#
# This is a RATCHET, not a gate. examples/examples-check-baseline.txt lists the
# files known to fail today, each with its reason. The sweep fails when:
#
#   * a file NOT in the baseline fails       -- a regression, fix it; or
#   * a file IN the baseline now passes      -- good news, drop it from the
#                                               baseline so it is protected.
#
# The second direction is what keeps the list honest: without it a baseline
# silently becomes a list of things nobody ever fixed.
set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-examples: $TUR not built" >&2; exit 2; }
BASELINE="examples/examples-check-baseline.txt"

known=""
if [ -f "$BASELINE" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|\#*) continue ;; esac
        known="$known ${line%% *}"
    done < "$BASELINE"
fi
is_known() { case " $known " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

PASS=0; FAIL=0
regressions=""; fixed=""

while IFS= read -r f; do
    if "$TUR" check "$f" >/dev/null 2>&1; then
        if is_known "$f"; then
            fixed="$fixed $f"; FAIL=$((FAIL + 1))
            echo "FAIL $f -- now passes, but is still listed in $BASELINE (remove it)"
        else
            PASS=$((PASS + 1)); echo "PASS $f"
        fi
    else
        if is_known "$f"; then
            PASS=$((PASS + 1)); echo "SKIP $f -- known-failing (see $BASELINE)"
        else
            regressions="$regressions $f"; FAIL=$((FAIL + 1))
            echo "FAIL $f -- tur check failed and it is not in $BASELINE"
            "$TUR" check "$f" 2>&1 | head -6 | sed 's/^/    /'
        fi
    fi
done <<EOF
$(find examples -name '*.tur' | sort)
EOF

echo ""
echo "examples check: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0
[ -n "$regressions" ] && echo "  regressions:$regressions"
[ -n "$fixed"       ] && echo "  now-passing, drop from baseline:$fixed"
exit 1
