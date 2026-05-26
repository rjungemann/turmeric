#!/usr/bin/env bash
# tests/lib/timeout.sh -- shared timeout helpers for all test harnesses.
#
# Source this file to get:
#   _tur_timeout_bin         -- detected timeout binary (timeout, gtimeout, or "")
#   tur_case_timeout_secs    -- per-case wall-clock budget in seconds (default: 30)
#   tur_suite_timeout_secs   -- whole-suite wall-clock budget in seconds (default: 300)
#   run_with_timeout <secs> <cmd...>
#       Run <cmd...> with a wall-clock limit of <secs> seconds.
#       Returns the command exit code, or 124 on timeout (GNU timeout convention).
#       Falls back to running without a limit when no timeout binary is available.
#   run_case_with_timeout <case-name> <secs> <cmd...>
#       Like run_with_timeout but prints a standardized FAIL line on timeout:
#         "FAIL <case-name> -- timed out after <secs>s"

# Honor caller overrides; otherwise use the documented defaults.
tur_case_timeout_secs="${tur_case_timeout_secs:-30}"
tur_suite_timeout_secs="${tur_suite_timeout_secs:-300}"

# Detect cross-platform timeout binary.
# On macOS, GNU coreutils installs as gtimeout (via Homebrew).
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then
    _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    _tur_timeout_bin="gtimeout"
fi

export _tur_timeout_bin tur_case_timeout_secs tur_suite_timeout_secs

run_with_timeout() {
    local secs="$1"; shift
    if [ -z "$_tur_timeout_bin" ] || [ "$secs" -le 0 ]; then
        "$@"
        return $?
    fi
    "$_tur_timeout_bin" "$secs" "$@"
}
export -f run_with_timeout

# run_case_with_timeout <case-name> <secs> <cmd...>
# On timeout (exit 124) prints "FAIL <case-name> -- timed out after <secs>s"
# and returns 124. Other failures return the command exit code without printing.
run_case_with_timeout() {
    local name="$1" secs="$2"; shift 2
    run_with_timeout "$secs" "$@"
    local rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "FAIL $name -- timed out after ${secs}s"
    fi
    return "$rc"
}
export -f run_case_with_timeout
