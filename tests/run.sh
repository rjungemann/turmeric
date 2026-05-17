#!/usr/bin/env bash
# tests/run.sh — fixture runner for tur (phase 0).
#
# Layout:
#   tests/fixtures/<name>/                 — happy-path fixture
#     input.tur (or <name>.tur)
#     expected.stdout
#     expected.c          (optional codegen snapshot)
#
#   tests/fixtures/errors/<name>/          — negative fixture
#     input.tur
#     expected.diag       (substring(s) that must appear in stderr; one per line)
#
# Pass: exits 0 with "PASS <name>". Any failure exits 1.

set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

PASS=0
FAIL=0
FAILED=()

# Performance plan item #1: compiler cache integration for build steps.
# Opt in with TUR_USE_CCACHE=1 (enabled by default here if ccache is available).
# The generated C is written to a deterministic path (/tmp/tur-build/<name>.c)
# so that ccache can actually produce cache hits across runs.
# CCACHE_NOHASHDIR=1 prevents ccache from hashing the source file directory,
# further improving hit rates for generated files.
TUR_USE_CCACHE="${TUR_USE_CCACHE:-1}"
BUILD_CC="${CC:-cc}"
if [ "$TUR_USE_CCACHE" = "1" ] && command -v ccache >/dev/null 2>&1; then
    BUILD_CC="ccache ${BUILD_CC}"
    export CCACHE_NOHASHDIR=1
fi

# Default compiler flags for test builds.
# Override with TUR_CC_FLAGS="-O1 -std=c99" for faster (but less safe) builds.
# NOTE: -O0 causes SIGTRAP on Apple Silicon; -O1 exposes latent UB in some
#       emitted functions missing a return path — keep -O2 for safety.
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -Lbuild/src}"

# T19: ThreadSanitizer (TSan) support.
# Set TUR_TSAN=1 to compile and run all fixtures with -fsanitize=thread.
# Fixtures whose directory contains a `requires.tsan` marker file are
# SKIPPED when TUR_TSAN is not set and run normally when it is set.
TUR_TSAN="${TUR_TSAN:-0}"
if [ "$TUR_TSAN" = "1" ]; then
    export TUR_CC_FLAGS="$TUR_CC_FLAGS -fsanitize=thread -g"
fi
export TUR_TSAN

# T19: Timeout support.
# `expected.timeout` in a fixture directory sets the per-fixture timeout in
# seconds.  Default is 10.  Set to 0 to disable the timeout for a fixture.
# Uses `timeout(1)` (GNU coreutils), `gtimeout` (Homebrew coreutils on macOS),
# or a Perl alarm(2) fallback when neither is available.
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then
    _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    _tur_timeout_bin="gtimeout"
fi
export _tur_timeout_bin

_run_timed() {
    local secs="$1"; shift
    if [ "$secs" -le 0 ] || [ -z "$_tur_timeout_bin" ]; then
        "$@"
    else
        "$_tur_timeout_bin" "$secs" "$@"
    fi
}
export -f _run_timed

# Performance plan item #3: avoid redundant emit-c work.
# "snapshot-only" means run emit-c only for fixtures that have expected.c.
# Set TUR_EMIT_C_MODE=always to force old behavior.
TUR_EMIT_C_MODE="${TUR_EMIT_C_MODE:-snapshot-only}"

# Performance plan item #2: parallel fixture execution.
# Override with TUR_TEST_JOBS=<n>; defaults to nproc*2 (capped at 32).
# Fixture workers are I/O-bound (process spawning, temp files, linking) so
# oversubscribing beyond the physical core count keeps pipelines saturated.
if [ -n "${TUR_TEST_JOBS:-}" ]; then
    JOBS="$TUR_TEST_JOBS"
else
    if command -v getconf >/dev/null 2>&1; then
        _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    elif command -v sysctl >/dev/null 2>&1; then
        _nproc="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
    else
        _nproc=4
    fi
    JOBS=$(( _nproc * 2 ))
fi

case "$JOBS" in
    ''|*[!0-9]*) JOBS=8 ;;
esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 32 ]; then JOBS=32; fi

RESULTS_DIR="$(mktemp -d -t tur-tests-results-XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# Optional regex filter for fixture names (relative path under tests/fixtures).
# Example: TUR_TEST_FILTER='^rc-auto-drop|^rc-ref-conversion$'
TUR_TEST_FILTER="${TUR_TEST_FILTER:-}"

# Optional sharding to split full suite across bounded timeout runs.
# Format: TUR_TEST_SHARD="1/8" (1-based index/total).
TUR_TEST_SHARD="${TUR_TEST_SHARD:-}"
SHARD_INDEX=0
SHARD_TOTAL=1
if [ -n "$TUR_TEST_SHARD" ]; then
    case "$TUR_TEST_SHARD" in
        */*)
            shard_left="${TUR_TEST_SHARD%/*}"
            shard_right="${TUR_TEST_SHARD#*/}"
            case "$shard_left" in ''|*[!0-9]*) shard_left=1 ;; esac
            case "$shard_right" in ''|*[!0-9]*) shard_right=1 ;; esac
            if [ "$shard_right" -lt 1 ]; then shard_right=1; fi
            if [ "$shard_left" -lt 1 ]; then shard_left=1; fi
            if [ "$shard_left" -gt "$shard_right" ]; then shard_left="$shard_right"; fi
            SHARD_TOTAL="$shard_right"
            SHARD_INDEX=$((shard_left - 1))
            ;;
    esac
fi

matches_filter() {
    local fixture_name="$1"
    if [ -z "$TUR_TEST_FILTER" ]; then
        return 0
    fi
    printf '%s\n' "$fixture_name" | grep -E -q "$TUR_TEST_FILTER"
}

matches_shard() {
    local ordinal="$1"
    if [ "$SHARD_TOTAL" -le 1 ]; then
        return 0
    fi
    [ $((ordinal % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]
}

write_result() {
    local kind="$1"
    local name="$2"
    local detail="$3"
    local log_file="$4"
    local id
    id="$(printf '%s' "$kind-$name" | tr '/ ' '__')"
    {
        printf '%s\n' "$kind"
        printf '%s\n' "$name"
        printf '%s\n' "$detail"
        printf '%s\n' "$log_file"
    } > "$RESULTS_DIR/$id.result"
    # Immediately print outcome so progress is visible during parallel runs.
    # Single-line echo calls are atomic on Linux/macOS (under PIPE_BUF),
    # so lines from concurrent workers do not interleave.
    if [ "$kind" = "PASS" ]; then
        echo "PASS $name"
    elif [ "$kind" = "FAIL" ]; then
        echo "FAIL $name${detail:+ — $detail}"
    fi
}

# ---------------------------------------------------------------------------
# Stamp-file caching (T2-C)
# After a fixture passes, record a stamp: content-hash of input.tur plus the
# mtime of the tur binary.  On the next run, if both are unchanged the fixture
# is skipped without rebuilding.
# Disable with TUR_FORCE=1 or by setting TUR_STAMP_CACHE="".
# Stamps are stored in tests/.stamp-cache/ (listed in .gitignore).
# NOTE: changes to stdlib/ files other than macros.tur are not tracked; run
#       with TUR_FORCE=1 after editing stdlib sources.
# ---------------------------------------------------------------------------
TUR_FORCE="${TUR_FORCE:-0}"
TUR_STAMP_CACHE="${TUR_STAMP_CACHE:-tests/.stamp-cache}"

_tur_hash_file() {
    local path="$1"
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$path" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then
        md5sum "$path" 2>/dev/null | awk '{print $1}'
    else
        echo "nohash"
    fi
}

_tur_mtime() {
    stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"
}

stamp_key() {
    local input="$1"
    local dir
    dir="$(dirname "$input")"
    # Incorporate the expected.c snapshot hash (if present) so that regenerating
    # snapshots invalidates the stamp and forces a fresh codegen check.
    local ec_hash=""
    [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
    echo "$(_tur_hash_file "$input")-${ec_hash}-$(_tur_mtime "$TUR")"
}

stamp_check() {
    local name="$1" input="$2"
    [ "$TUR_FORCE" = "1" ] && return 1
    [ -z "$TUR_STAMP_CACHE" ] && return 1
    local sf="$TUR_STAMP_CACHE/$(printf '%s' "$name" | tr '/ ' '__').stamp"
    [ -f "$sf" ] || return 1
    [ "$(cat "$sf")" = "$(stamp_key "$input")" ]
}

stamp_write() {
    local name="$1" input="$2"
    [ -z "$TUR_STAMP_CACHE" ] && return
    mkdir -p "$TUR_STAMP_CACHE"
    local sf="$TUR_STAMP_CACHE/$(printf '%s' "$name" | tr '/ ' '__').stamp"
    stamp_key "$input" > "$sf"
}
# ---------------------------------------------------------------------------

run_happy() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else echo "SKIP $name (no input)" ; return; fi

    # T19: Skip fixtures requiring TSan when TSan is not active.
    if [ -f "$dir/requires.tsan" ] && [ "$TUR_TSAN" != "1" ]; then
        write_result "PASS" "$name" "(tsan-skipped)" ""
        return
    fi

    # T19: Read per-fixture timeout (default: 10 seconds; 0 = unlimited).
    local fixture_timeout=10
    if [ -f "$dir/expected.timeout" ]; then
        local _t
        _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    local out_dir="$dir"
    local actual_stdout="$out_dir/actual.stdout"
    local actual_stderr="$out_dir/actual.stderr"
    local actual_c="$out_dir/actual.c"
    local log_file="$RESULTS_DIR/$(printf '%s' "happy-$name" | tr '/ ' '__').log"
    local needs_codegen_check=0

    if [ -f "$dir/expected.c" ]; then
        needs_codegen_check=1
    fi

    # Stamp fast-path: skip if input, expected.c, and tur binary are all
    # unchanged since the last passing run.
    if stamp_check "$name" "$input"; then
        write_result "PASS" "$name" "" ""
        return
    fi

    # Read per-fixture compiler flags if present
    local fixture_flags=""
    if [ -f "$dir/flags" ]; then
        fixture_flags=$(cat "$dir/flags")
    fi

    if [ "$TUR_EMIT_C_MODE" = "always" ] || [ "$needs_codegen_check" -eq 1 ]; then
        "$TUR" $fixture_flags emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
        if [ $? -ne 0 ]; then
            {
                echo "FAIL $name — tur emit-c failed"
                cat "$out_dir/actual.stderr"
            } > "$log_file"
            write_result "FAIL" "$name" "emit-c failed" "$log_file"
            return
        fi
    fi

    local exe
    exe=$(mktemp -t tur-test-XXXXXX)
    CC="$BUILD_CC" "$TUR" $fixture_flags build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
    if [ $? -ne 0 ]; then
        {
            echo "FAIL $name — tur build failed"
            cat "$out_dir/actual.stderr"
        } > "$log_file"
        write_result "FAIL" "$name" "build failed" "$log_file"
        rm -f "$exe"
        return
    fi

    if [ -f "$dir/input.stdin" ]; then
        _run_timed "$fixture_timeout" "$exe" < "$dir/input.stdin" > "$actual_stdout" 2>> "$actual_stderr"
    else
        _run_timed "$fixture_timeout" "$exe" > "$actual_stdout" 2>> "$actual_stderr"
    fi
    local rc=$?
    rm -f "$exe"

    local expected_exit="0"
    if [ -f "$dir/expected.exit" ]; then
        expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")
    fi

    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null; then
            {
                echo "FAIL $name — stdout mismatch"
                diff -u "$dir/expected.stdout" "$actual_stdout" | sed 's/^/    /'
            } > "$log_file"
            write_result "FAIL" "$name" "stdout mismatch" "$log_file"
            return
        fi
    fi

    if [ "$needs_codegen_check" -eq 1 ]; then
        if ! diff -u "$dir/expected.c" "$actual_c" > /dev/null; then
            {
                echo "FAIL $name — codegen mismatch"
                diff -u "$dir/expected.c" "$actual_c" | sed 's/^/    /'
            } > "$log_file"
            write_result "FAIL" "$name" "codegen mismatch" "$log_file"
            return
        fi
    fi

    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            {
                echo "FAIL $name — expected nonzero exit, got 0"
            } > "$log_file"
            write_result "FAIL" "$name" "expected nonzero exit" "$log_file"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            {
                echo "FAIL $name — program exited $rc (expected $expected_exit)"
            } > "$log_file"
            write_result "FAIL" "$name" "exit $rc, expected $expected_exit" "$log_file"
            return
        fi
    fi

    if [ -f "$dir/expected.stderr" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$actual_stderr"; then
                {
                    echo "FAIL $name — expected stderr substring not found:"
                    echo "    $needle"
                } >> "$log_file"
                missing=1
            fi
        done < "$dir/expected.stderr"
        if [ $missing -ne 0 ]; then
            {
                echo "    actual stderr:"
                sed 's/^/      /' "$actual_stderr"
            } >> "$log_file"
            write_result "FAIL" "$name" "stderr mismatch" "$log_file"
            return
        fi
    fi

    stamp_write "$name" "$input"
    write_result "PASS" "$name" "" ""
}

run_negative() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input="$dir/input.tur"
    [ -f "$input" ] || { echo "SKIP $name (no input)"; return; }

    local log_file="$RESULTS_DIR/$(printf '%s' "neg-$name" | tr '/ ' '__').log"

    # Stamp fast-path: skip if input + tur binary unchanged since last PASS.
    if stamp_check "$name" "$input"; then
        write_result "PASS" "$name" "" ""
        return
    fi

    local neg_flags=""
    if [ -f "$dir/flags" ]; then
        neg_flags=$(cat "$dir/flags")
    fi
    $TUR $neg_flags emit-c "$input" > /dev/null 2> "$dir/actual.stderr"
    local rc=$?
    if [ $rc -eq 0 ]; then
        {
            echo "FAIL $name — expected error, but tur exited 0"
        } > "$log_file"
        write_result "FAIL" "$name" "expected error, got success" "$log_file"
        return
    fi

    if [ -f "$dir/expected.diag" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$dir/actual.stderr"; then
                {
                    echo "FAIL $name — expected diagnostic substring not found:"
                    echo "    $needle"
                } >> "$log_file"
                missing=1
            fi
        done < "$dir/expected.diag"
        if [ $missing -ne 0 ]; then
            {
                echo "    actual stderr:"
                sed 's/^/      /' "$dir/actual.stderr"
            } >> "$log_file"
            write_result "FAIL" "$name" "diagnostic mismatch" "$log_file"
            return
        fi
    fi

    stamp_write "$name" "$input"
    write_result "PASS" "$name" "" ""
}

run_happy_worker() {
    run_happy "$1"
}

run_negative_worker() {
    run_negative "$1"
}

export TUR BUILD_CC RESULTS_DIR TUR_EMIT_C_MODE
export TUR_TEST_FILTER
export TUR_TEST_SHARD SHARD_INDEX SHARD_TOTAL
export TUR_FORCE TUR_STAMP_CACHE
export TUR_TSAN _tur_timeout_bin
export -f matches_filter matches_shard write_result run_happy run_negative run_happy_worker run_negative_worker
export -f _tur_hash_file _tur_mtime stamp_key stamp_check stamp_write _run_timed

# Happy fixtures: tests/fixtures/* except tests/fixtures/errors
shopt -s nullglob
HAPPY_DIRS=()
fixture_ordinal=0
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if matches_filter "$name" && matches_shard "$fixture_ordinal"; then
        HAPPY_DIRS+=("$d")
    fi
    fixture_ordinal=$((fixture_ordinal + 1))
done

if [ ${#HAPPY_DIRS[@]} -gt 0 ]; then
    HAPPY_LIST_FILE="$RESULTS_DIR/happy_dirs.list"
    printf '%s\n' "${HAPPY_DIRS[@]}" > "$HAPPY_LIST_FILE"
    xargs -P "$JOBS" -I{} bash -c 'run_happy_worker "$@"' _ {} < "$HAPPY_LIST_FILE" 2>/dev/null
fi

# Error fixtures
ERROR_DIRS=()
error_ordinal=0
for d in tests/fixtures/errors/*/; do
    d="${d%/}"
    [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if matches_filter "$name" && matches_shard "$error_ordinal"; then
        ERROR_DIRS+=("$d")
    fi
    error_ordinal=$((error_ordinal + 1))
done

if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    ERROR_LIST_FILE="$RESULTS_DIR/error_dirs.list"
    printf '%s\n' "${ERROR_DIRS[@]}" > "$ERROR_LIST_FILE"
    xargs -P "$JOBS" -I{} bash -c 'run_negative_worker "$@"' _ {} < "$ERROR_LIST_FILE" 2>/dev/null
fi

for result_file in "$RESULTS_DIR"/*.result; do
    [ -f "$result_file" ] || continue
    kind="$(sed -n '1p' "$result_file")"
    name="$(sed -n '2p' "$result_file")"
    detail="$(sed -n '3p' "$result_file")"
    log_file="$(sed -n '4p' "$result_file")"

    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
        # PASS line already printed by the worker for live progress.
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name ($detail)")
        if [ -n "$log_file" ] && [ -f "$log_file" ]; then
            cat "$log_file"
        else
            echo "FAIL $name — $detail"
        fi
    fi
done

echo
echo "summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
