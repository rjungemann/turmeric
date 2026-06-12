#!/usr/bin/env bash
# tests/run-turi-full-prelude.sh -- prereq 3c gate: TUR_TURI_FULL_PRELUDE.
#
# The interpreter's default prelude covers the typed-stdlib core but carves out
# contract/mutmap/json/schema (docs/turi-preload-carve-out.txt).  Setting
# TUR_TURI_FULL_PRELUDE=1 opts into loading those on top of the default prelude
# so the carved bucket can be iterated/measured under --interpret.  This gate
# asserts the flag actually toggles a carved module's availability:
#
#   flag OFF -> `mutmap-new` is unbound  (rc=1, "unknown function")
#   flag ON  -> `mutmap-new` is defined and runs (rc=0)
#
# Exit status: 0 if both directions hold, else 1.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "FAIL turi-full-prelude -- $TUR not built"; exit 1; }

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

prog="$(mktemp -t tur-fullprelude-XXXXXX).tur"
printf '(defn main [] : int\n  (let [m (mutmap-new)] 0))\n' > "$prog"

fail=0

# Flag OFF: the carved module is not loaded, so mutmap-new is unknown.
off_err="$("$TUR" --interpret "$prog" 2>&1 >/dev/null)"; off_rc=$?
if [ "$off_rc" -eq 0 ]; then
    echo "FAIL turi-full-prelude -- mutmap-new resolved WITHOUT the flag (default prelude leaked a carved module?)"
    fail=1
elif ! printf '%s' "$off_err" | grep -q "mutmap-new"; then
    echo "FAIL turi-full-prelude -- flag-off error did not mention mutmap-new: $off_err"
    fail=1
fi

# Flag ON: the carved module loads, so mutmap-new is defined and runs.
on_err="$(TUR_TURI_FULL_PRELUDE=1 "$TUR" --interpret "$prog" 2>&1 >/dev/null)"; on_rc=$?
if [ "$on_rc" -ne 0 ]; then
    echo "FAIL turi-full-prelude -- mutmap-new still failed WITH the flag (rc=$on_rc): $on_err"
    fail=1
fi

# A non-"1" value must NOT enable it (strict =1 convention).
junk_err="$(TUR_TURI_FULL_PRELUDE=yes "$TUR" --interpret "$prog" 2>&1 >/dev/null)"; junk_rc=$?
if [ "$junk_rc" -eq 0 ]; then
    echo "FAIL turi-full-prelude -- TUR_TURI_FULL_PRELUDE=yes wrongly enabled the full prelude"
    fail=1
fi

rm -f "$prog"
if [ "$fail" -ne 0 ]; then exit 1; fi
echo "PASS turi-full-prelude"
exit 0
