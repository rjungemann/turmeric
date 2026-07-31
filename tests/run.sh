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

# Isolate the test suite from any globally exported TUR_STDLIB_DIR (e.g. from
# a mise-managed global `turmeric` install). Otherwise the locally built
# ./build/tur loads stdlib sources from the global install path, and
# __tur_autolink__ references (e.g. src/runtime/hamt.c) resolve into a tree
# that has no such files, spuriously failing the whole suite.
unset TUR_STDLIB_DIR

# UC-3 (user-config-experiments-plan): the compiler now reads a user-level
# experiments file at $XDG_CONFIG_HOME/turmeric/experiments.tur (fallback
# $HOME/.config/turmeric/experiments.tur). A contributor whose real home
# directory carries one would otherwise see local-only experiment enables
# leak into the suite. Point XDG_CONFIG_HOME at an empty temp dir (and clear
# HOME's fallback effect by keeping XDG set) so every run sees no user file.
_TUR_EMPTY_XDG="$(mktemp -d "${TMPDIR:-/tmp}/tur-xdg-empty.XXXXXX")"
export XDG_CONFIG_HOME="$_TUR_EMPTY_XDG"
# NOTE: the EXIT trap that removes this dir is installed alongside the
# RESULTS_DIR cleanup below (a later `trap ... EXIT` would otherwise replace
# an earlier one).

# Overridable so a non-default build tree can be tested without editing this
# file -- notably Windows, where the binary is build-win/tur.exe.
TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

# Identity of the binary under test, re-checked at the end of the run.
#
# Fixtures exec $TUR directly out of the build tree, so a rebuild that lands
# mid-run swaps the compiler underneath the suite. During the link window the
# file exists but is not yet executable, and every fixture dispatched in that
# window dies with `Permission denied` -- which this harness reports as
# "build failed". A batch of those reads as a compiler regression and costs a
# full investigation before the cause (a concurrent `cmake --build`) turns up.
#
# `ls -ln` rather than stat(1): the -c/-f format flags are GNU/BSD-specific,
# while the size and mtime columns of `ls -ln` are portable enough to compare
# as an opaque string. A relink that somehow preserved both would slip through;
# that is not a case worth more machinery.
TUR_STAMP_START="$(ls -ln "$TUR" 2>/dev/null)"

# A handful of fixtures write to a literal "/tmp/..." from inline-C fopen. On
# POSIX that always exists; a compiled Windows binary resolves "/tmp" against the
# current drive (C:\tmp), which is not present by default. Create it so those
# fixtures behave the same as everywhere else. Guarded on MSYSTEM so this is a
# no-op off Windows.
case "${MSYSTEM:-}" in
  UCRT64|MINGW64|CLANG64|MINGW32) mkdir -p /c/tmp 2>/dev/null || true ;;
esac

# Force server fixtures to bind 127.0.0.1 instead of INADDR_ANY. On Windows this
# stops the Defender Firewall "allow this app" dialog from popping for every
# freshly-built fixture binary; elsewhere it is a harmless tightening (the
# fixtures are same-process loopback tests). The stdlib socket/httpd listen code
# reads this env at runtime and only then binds loopback.
export TUR_BIND_LOOPBACK=1

# TI8 (turi-parity-post-v1-plan): CI ratchet -- fail fast if any EX_* expression
# kind the compiler emits has no `case` arm in src/turi/eval.c and is not a
# documented carve-out (docs/artifacts/turi-carve-out.txt).  Cheap, deterministic, and
# keeps the interpreter parity gap from silently growing.  Opt out with
# TUR_SKIP_PARITY_CHECK=1 (e.g. when hacking on eval.c mid-change).
if [ "${TUR_SKIP_PARITY_CHECK:-0}" != "1" ] && command -v python3 >/dev/null 2>&1; then
    if ! python3 tools/check_turi_parity.py; then
        echo "tests: turi parity check failed (see above); aborting." >&2
        exit 1
    fi
    # Prereq 3a (turi-open-reports-prereqs.md): module-preload parity -- every
    # module the compiled path auto-loads is either in the --interpret prelude
    # or carved out with a rationale in docs/artifacts/turi-preload-carve-out.txt.  Keeps
    # the harness-flip "missing native" bucket from silently growing.
    if ! python3 tools/check_turi_native_parity.py; then
        echo "tests: turi native-parity check failed (see above); aborting." >&2
        exit 1
    fi
fi

# R4 (carrier-crossing-recovery-routing-plan): the audit registry is the single
# source of truth for which carrier<->concrete crossings are routed.  A new
# chokepoint call site that forgot its audit row (or a drifted/stale registry)
# fails here.  Independent of the turi-parity ratchets above so it still runs
# when those are skipped or unrelated.  Opt out with TUR_SKIP_CROSSING_CHECK=1.
if [ "${TUR_SKIP_CROSSING_CHECK:-0}" != "1" ] && command -v python3 >/dev/null 2>&1; then
    if ! python3 tools/check_crossing_routing.py --quiet; then
        echo "tests: crossing-routing audit check failed (see above); aborting." >&2
        exit 1
    fi
fi

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
# The `-L` for libturi.a is derived from where $TUR actually lives, not
# hardcoded to build/src -- otherwise a non-default build tree (Windows uses
# build-win/) can't resolve the `-lturi` autolink that reactor/async fixtures
# emit, and every one of them fails to link.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

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
# Override with TUR_TEST_JOBS=<n>; defaults to physical core count (capped at 8).
# The auto-detected default is capped at physical cores (not 2x) to avoid
# flooding syspolicyd on macOS with simultaneous new-binary executions from
# requires.compiled fixtures.  An *explicit* TUR_TEST_JOBS is an intentional
# override and is honored uncapped -- a 16/32-core CI runner should be able to
# use its cores.
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
    JOBS=$(( _nproc ))
    # Cap the auto-detected value only; an explicit TUR_TEST_JOBS bypasses this.
    if [ "$JOBS" -gt 8 ]; then JOBS=8; fi
fi

case "$JOBS" in
    ''|*[!0-9]*) JOBS=4 ;;
esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi

RESULTS_DIR="$(mktemp -d -t tur-tests-results-XXXXXX)"
trap 'rm -rf "$RESULTS_DIR" "$_TUR_EMPTY_XDG"' EXIT

# A killed or interrupted run must NEVER print a success-looking summary.
# Without this guard, a SIGINT/SIGTERM that the parent shell survives (e.g. the
# harness signals only the xargs workers, or the run is cut short for time)
# would fall straight through to the "summary: N passed, 0 failed" tally below
# and report a *partial* run as green.  Trap the catchable signals and bail with
# a loud, non-zero status; the completeness check at the very end is the backstop
# for the cases a trap cannot see (SIGKILLed workers).  _INTERRUPTED is also read
# by that final check.
_INTERRUPTED=0
_abort_on_signal() {
    _INTERRUPTED=1
    echo
    echo "ABORTED: run.sh caught a signal before finishing -- results are PARTIAL, NOT a pass." >&2
    # 130 = 128 + SIGINT(2); a conventional "interrupted" status that is clearly
    # not 0 (success) and not 1 (test failures).
    exit 130
}
trap _abort_on_signal INT TERM

# Optional regex filter for fixture names (relative path under tests/fixtures).
# Example: TUR_TEST_FILTER='^rc-auto-drop|^rc-ref-conversion$'
TUR_TEST_FILTER="${TUR_TEST_FILTER:-}"

# Optional named sub-suite for faster developer feedback / CI fan-out.
# Groups are defined by file/dir presence (robust), not fragile name regexes:
#   all|<empty> -- everything (default)
#   happy       -- positive fixtures only (tests/fixtures/* minus errors/)
#   errors      -- negative fixtures only (tests/fixtures/errors/*)
#   snapshots   -- positive fixtures carrying a codegen snapshot (expected.c)
# Compose freely with TUR_TEST_FILTER (regex) and TUR_TEST_SHARD.
TUR_TEST_SUITE="${TUR_TEST_SUITE:-}"
case "$TUR_TEST_SUITE" in
    ''|all|happy|errors|snapshots) ;;
    *)
        echo "run.sh: unknown TUR_TEST_SUITE='$TUR_TEST_SUITE'" >&2
        echo "  valid: all (default), happy, errors, snapshots" >&2
        exit 2
        ;;
esac

# Admit a fixture into the current suite.  $1 = kind (happy|error); $2 = dir.
# Ordinals are still assigned over the FULL discovery order (see below), so
# suite selection composes with sharding without shifting shard membership.
suite_admits() {
    case "$TUR_TEST_SUITE" in
        ''|all)    return 0 ;;
        happy)     [ "$1" = "happy" ] ;;
        errors)    [ "$1" = "error" ] ;;
        snapshots) [ "$1" = "happy" ] && [ -f "$2/expected.c" ] ;;
        *)         return 0 ;;
    esac
}

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

# Performance optimization: cache the compiler binary modification time once
# at startup so we do not spawn a redundant stat process for every single fixture.
export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() {
    local input="$1"
    local dir
    dir="$(dirname "$input")"
    # Incorporate the expected.c snapshot hash (if present) so that regenerating
    # snapshots invalidates the stamp and forces a fresh codegen check.
    local ec_hash=""
    [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
    echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}"
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

    # hook.sh: if present, delegate entirely to the fixture-supplied script.
    # The script is invoked with TUR, CC, and TUR_CC_FLAGS in the environment.
    # It should write its stdout to a file named `actual.stdout` in a temp dir
    # that the runner passes as $1, and exit 0 on success, nonzero on failure.
    if [ -f "$dir/hook.sh" ]; then
        local hook_tmp
        hook_tmp=$(mktemp -d)
        local hook_log="$hook_tmp/hook.log"
        local actual_hook_stdout="$hook_tmp/actual.stdout"
        # requires.no-leak-check: honor the marker on the hook.sh path too.
        # LeakSanitizer aborts via _exit(), which skips stdio flushing -- a
        # buffered final line (e.g. "done") would be lost and the snapshot
        # would mismatch.  Disable leak detection for the spawned program,
        # mirroring the standard runner below.
        local hook_asan="$ASAN_OPTIONS"
        if [ -f "$dir/requires.no-leak-check" ]; then
            hook_asan="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
        fi
        TUR="$TUR" CC="$BUILD_CC" TUR_CC_FLAGS="$TUR_CC_FLAGS" ASAN_OPTIONS="$hook_asan" \
            bash "$dir/hook.sh" "$hook_tmp" > "$actual_hook_stdout" 2> "$hook_log"
        local hook_rc=$?
        if [ $hook_rc -ne 0 ]; then
            { echo "FAIL $name -- hook.sh exited $hook_rc"; cat "$hook_log"; } > "$hook_log.final"
            write_result "FAIL" "$name" "hook.sh failed (exit $hook_rc)" "$hook_log.final"
            rm -rf "$hook_tmp"
            return
        fi
        if [ -f "$dir/expected.stdout" ]; then
            if ! diff -u "$dir/expected.stdout" "$actual_hook_stdout" > /dev/null; then
                { echo "FAIL $name -- stdout mismatch"; diff -u "$dir/expected.stdout" "$actual_hook_stdout" | sed 's/^/    /'; } > "$hook_log.final"
                write_result "FAIL" "$name" "stdout mismatch" "$hook_log.final"
                rm -rf "$hook_tmp"
                return
            fi
        fi
        rm -rf "$hook_tmp"
        write_result "PASS" "$name" "" ""
        return
    fi

    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else echo "SKIP $name (no input)" ; write_result "PASS" "$name" "(no input -- skipped)" "" ; return; fi

    # T19: Skip fixtures requiring TSan when TSan is not active.
    if [ -f "$dir/requires.tsan" ] && [ "$TUR_TSAN" != "1" ]; then
        write_result "PASS" "$name" "(tsan-skipped)" ""
        return
    fi

    # Skip fixtures owned by a dedicated ctest target (e.g. eval-import
    # has its own tur_eval_import test with custom -I/-L flags).
    if [ -f "$dir/requires.dedicated-runner" ]; then
        write_result "PASS" "$name" "(dedicated-runner-skipped)" ""
        return
    fi

    # Skip fixtures that load from the optional sibling turmeric-spices
    # repo when that directory isn't present. See CLAUDE.md "Optional
    # dependencies" for how to enable.
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        write_result "PASS" "$name" "(spices-skipped)" ""
        return
    fi

    # turi-session-types-plan (Slice B): interpreter-only fixtures whose peer
    # runs as a `tur --interpret` async fiber over the cooperative session
    # runtime.  They are owned by tests/run-turi.sh; the compiled suite skips
    # them (a cooperative async fiber cannot rendezvous on the compiled pthread
    # session channel).  Distinct from requires.interp, which routes through the
    # compiling `tur run` path, not `--interpret`.
    if [ -f "$dir/requires.interp-only" ]; then
        write_result "PASS" "$name" "(interp-only-skipped)" ""
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
    # Default: compiled. All fixtures run through tur build unless they
    # carry a requires.interp marker (reserved for future interpreter-only
    # tests).  Under TUR_TSAN=1 compiled mode is always forced.
    local needs_compiled=1

    if [ -f "$dir/expected.c" ]; then
        needs_codegen_check=1
    fi

    # requires.interp: override compiled default and use the interpreter.
    # requires.compiled is kept for documentation but is now a no-op.
    if [ -f "$dir/requires.interp" ] && [ "$TUR_TSAN" != "1" ]; then
        needs_compiled=0
    fi

    # requires.no-leak-check: run the compiled binary with LeakSanitizer
    # disabled.  Reserved for fixtures whose program intentionally registers
    # process-lifetime closures (e.g. reactor callbacks) that the caller never
    # frees -- mirroring the interpreter ASAN policy in CLAUDE.md.  The
    # compiler/codegen path itself is still leak-checked (emit-c/build above).
    local run_env=()
    if [ -f "$dir/requires.no-leak-check" ]; then
        run_env=(env "ASAN_OPTIONS=${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0")
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

    # Read per-fixture run arguments if present (space-separated, one line).
    # For the compiled path these are passed directly to the binary.
    # For the interpreter path they are passed after -- to `tur run`.
    local run_args_arr=()
    if [ -f "$dir/run.args" ]; then
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            run_args_arr+=("$_ra")
        done < "$dir/run.args"
    fi

    # Codegen phase: wrap in _run_timed.  A compiler infinite loop in emit-c was
    # previously UNTIMED -- it stalled the worker (and the whole suite) forever,
    # because the per-fixture timeout only covered running the compiled binary,
    # never the emit-c/build phases.  A hung emit-c now FAILs on timeout instead.
    if [ "$TUR_EMIT_C_MODE" = "always" ] || [ "$needs_codegen_check" -eq 1 ]; then
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
        _emit_rc=$?
        if [ $_emit_rc -ne 0 ]; then
            {
                if [ $_emit_rc -eq 124 ]; then
                    echo "FAIL $name — tur emit-c timed out (>${fixture_timeout}s)"
                else
                    echo "FAIL $name — tur emit-c failed"
                fi
                cat "$out_dir/actual.stderr"
            } > "$log_file"
            write_result "FAIL" "$name" "emit-c failed" "$log_file"
            return
        fi
    fi

    local rc
    if [ "$needs_compiled" -eq 1 ]; then
        # Compiled path: build a native binary and run it.
        # Spawns cc + a new executable; triggers syspolicyd on macOS.
        local exe
        exe=$(mktemp -t tur-test-XXXXXX)
        # Build phase (compiler frontend + cc/ccache): also wrap in _run_timed.
        # This was untimed too, so a hung codegen or a wedged C compiler stalled
        # the suite indefinitely.  A timeout here is now a FAIL, not a hang.
        CC="$BUILD_CC" _run_timed "$fixture_timeout" "$TUR" $fixture_flags build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
        _build_rc=$?
        if [ $_build_rc -ne 0 ]; then
            {
                if [ $_build_rc -eq 124 ]; then
                    echo "FAIL $name — tur build timed out (>${fixture_timeout}s)"
                else
                    echo "FAIL $name — tur build failed"
                fi
                cat "$out_dir/actual.stderr"
            } > "$log_file"
            write_result "FAIL" "$name" "build failed" "$log_file"
            rm -f "$exe"
            return
        fi
        if [ -f "$dir/input.stdin" ]; then
            _run_timed "$fixture_timeout" "${run_env[@]}" "$exe" "${run_args_arr[@]}" < "$dir/input.stdin" > "$actual_stdout" 2>> "$actual_stderr"
        else
            _run_timed "$fixture_timeout" "${run_env[@]}" "$exe" "${run_args_arr[@]}" > "$actual_stdout" 2>> "$actual_stderr"
        fi
        rc=$?
        rm -f "$exe"
    else
        # Interpreter path: run via `tur run` -- no cc invocation, no new binary,
        # no syspolicyd hit.  This is the default for all fixtures that do not
        # have a requires.compiled marker.
        if [ "${#run_args_arr[@]}" -gt 0 ]; then
            if [ -f "$dir/input.stdin" ]; then
                _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" -- "${run_args_arr[@]}" \
                    < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr"
            else
                _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" -- "${run_args_arr[@]}" \
                    > "$actual_stdout" 2> "$actual_stderr"
            fi
        elif [ -f "$dir/input.stdin" ]; then
            _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr"
        else
            _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                > "$actual_stdout" 2> "$actual_stderr"
        fi
        rc=$?
    fi

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
    [ -f "$input" ] || { echo "SKIP $name (no input)"; write_result "PASS" "$name" "(no input -- skipped)" "" ; return; }

    # Skip negative fixtures that load from the optional sibling turmeric-spices
    # repo when that directory isn't present (mirrors the happy-path guard above).
    # Without this, a spices-dependent error fixture fails for any contributor
    # who has not cloned the sibling repo -- the diagnostic differs because the
    # spice import never resolves. See CLAUDE.md "Optional dependencies".
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        write_result "PASS" "$name" "(spices-skipped)" ""
        return
    fi

    # Per-fixture timeout (default 10s) -- negative fixtures only emit-c, but an
    # untimed front-end hang stalled the suite just like the happy path did.
    local fixture_timeout=10
    if [ -f "$dir/expected.timeout" ]; then
        local _nt; _nt=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_nt" in [0-9]*) fixture_timeout=$_nt ;; esac
    fi

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
    _run_timed "$fixture_timeout" $TUR $neg_flags emit-c "$input" > /dev/null 2> "$dir/actual.stderr"
    local rc=$?
    if [ $rc -eq 124 ]; then
        {
            echo "FAIL $name — tur emit-c timed out (>${fixture_timeout}s)"
        } > "$log_file"
        write_result "FAIL" "$name" "emit-c timed out" "$log_file"
        return
    fi
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
export TUR_TSAN _tur_timeout_bin TUR_MTIME
export -f matches_filter matches_shard write_result run_happy run_negative run_happy_worker run_negative_worker
export -f _tur_hash_file _tur_mtime stamp_key stamp_check stamp_write _run_timed

# Happy fixtures: tests/fixtures/* except tests/fixtures/errors, PLUS the
# fixtures one level down inside a GROUP directory.
#
# A group dir (typed/, typed-slots/, recursive-types/, ...) holds fixtures
# rather than being one -- it has no input.tur of its own, so the top-level
# scan skipped both it and its children, and those children were compiled by
# NO harness (run-turi.sh scans this deep, but only interprets).  That is
# exactly where two latent miscompiles sat undisturbed until the J3 jit
# harness compiled them: docs/archive/typed-result-map-cps-clone-struct-assign.md
# and docs/archive/typed-slots-nested-specialization-float-garbage.md.  Both
# are fixed, so this scan now covers them and the class cannot re-hide.
#
# Detection is structural, not a hard-coded list, and deliberately STRICT: a
# dir is a group only when it holds NOTHING BUT subdirectories (no regular
# file of its own -- no input.tur, expected.*, build.tur, hook.sh, marker, or
# loose *.tur) AND at least one of those subdirectories carries an input.tur.
#
# Both halves are load-bearing.  A project fixture driven by build.tur/hook.sh
# rather than input.tur (workspace-ls2/, spice-resolver-ok/, reader-macros-*)
# has regular files, so the first half keeps it a fixture.  A project fixture
# whose only entry is a source dir (module-transitive-imports/src/) passes the
# first half but fails the second, because src/ has no input.tur.  A looser
# rule silently DROPPED ~34 such fixtures from the suite while still reporting
# 0 failed -- the same invisible-coverage-loss this whole change exists to
# close, so the set inclusion is asserted below rather than assumed.
shopt -s nullglob
FIXTURE_DIRS=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] || continue
    # "holds no regular file of its own" via a plain glob -- BSD/macOS find has
    # no portable `-print -quit`, and this file is otherwise careful to stay
    # macOS-clean (stat -f first, sysctl for core count, gtimeout fallback).
    _is_group=0
    _has_own_file=0
    for _f in "$d"/*; do
        [ -f "$_f" ] && { _has_own_file=1; break; }
    done
    if [ "$_has_own_file" = 0 ]; then
        for sub in "$d"/*/; do
            [ -f "${sub}input.tur" ] && { _is_group=1; break; }
        done
    fi
    if [ "$_is_group" = 0 ]; then
        FIXTURE_DIRS+=("$d")            # a fixture in its own right
    else
        for sub in "$d"/*/; do          # a group dir: take its children
            sub="${sub%/}"
            [ -d "$sub" ] || continue
            [ -f "$sub/input.tur" ] || [ -f "$sub/$(basename "$sub").tur" ] || continue
            FIXTURE_DIRS+=("$sub")
        done
    fi
done

HAPPY_DIRS=()
fixture_ordinal=0
for d in "${FIXTURE_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if suite_admits happy "$d" && matches_filter "$name" && matches_shard "$fixture_ordinal"; then
        HAPPY_DIRS+=("$d")
    fi
    fixture_ordinal=$((fixture_ordinal + 1))
done

HAPPY_XARGS_RC=0
if [ ${#HAPPY_DIRS[@]} -gt 0 ]; then
    HAPPY_LIST_FILE="$RESULTS_DIR/happy_dirs.list"
    printf '%s\n' "${HAPPY_DIRS[@]}" > "$HAPPY_LIST_FILE"
    # Capture xargs' exit status.  Workers always exit 0 (test failures are
    # recorded in .result files, not via exit code), so a non-zero rc here means
    # xargs or a worker was killed by a signal -- i.e. the run was interrupted.
    xargs -P "$JOBS" -I{} bash -c 'run_happy_worker "$@"' _ {} < "$HAPPY_LIST_FILE" 2>/dev/null || HAPPY_XARGS_RC=$?
fi

# Error fixtures
ERROR_DIRS=()
error_ordinal=0
for d in tests/fixtures/errors/*/; do
    d="${d%/}"
    [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if suite_admits error "$d" && matches_filter "$name" && matches_shard "$error_ordinal"; then
        ERROR_DIRS+=("$d")
    fi
    error_ordinal=$((error_ordinal + 1))
done

ERROR_XARGS_RC=0
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    ERROR_LIST_FILE="$RESULTS_DIR/error_dirs.list"
    printf '%s\n' "${ERROR_DIRS[@]}" > "$ERROR_LIST_FILE"
    xargs -P "$JOBS" -I{} bash -c 'run_negative_worker "$@"' _ {} < "$ERROR_LIST_FILE" 2>/dev/null || ERROR_XARGS_RC=$?
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

# Completeness guard -- the core invariant: a partial run is NEVER a pass.
# Every dispatched fixture writes exactly one .result file (PASS or FAIL, incl.
# every skip path).  If fewer results landed than we dispatched, or if either
# xargs reported a signal-killed worker, or a signal trap fired, then the run
# was cut short and the tally above is partial.  Refuse to print the
# success-looking "summary: N passed, 0 failed" line in that case.
DISPATCHED=$(( ${#HAPPY_DIRS[@]} + ${#ERROR_DIRS[@]} ))
RESULT_COUNT=$(find "$RESULTS_DIR" -maxdepth 1 -name '*.result' 2>/dev/null | wc -l | tr -d '[:space:]')
: "${RESULT_COUNT:=0}"

echo

# Did the compiler change underneath us? See TUR_STAMP_START above. Reported
# before the tallies so it is the first thing read, since it invalidates them.
TUR_STAMP_END="$(ls -ln "$TUR" 2>/dev/null)"
if [ "$TUR_STAMP_START" != "$TUR_STAMP_END" ]; then
    echo "WARNING: $TUR changed while this run was in progress."
    echo "  before: $TUR_STAMP_START"
    echo "  after:  $TUR_STAMP_END"
    echo "  Something rebuilt the compiler mid-run (a concurrent 'cmake --build',"
    echo "  most likely). Fixtures exec this binary directly, so any failure"
    echo "  above -- especially a batch of 'build failed' -- is an artifact of"
    echo "  the swap, not a result. Re-run with nothing else building."
    if [ $FAIL -ne 0 ]; then
        # Same principle as the completeness guard: a run that cannot be
        # trusted is not a pass and not an ordinary failure.
        echo "  $FAIL failure(s) recorded -- THIS IS NOT A VALID RUN."
        exit 2
    fi
fi

if [ "$_INTERRUPTED" -ne 0 ] \
   || [ "$HAPPY_XARGS_RC" -ne 0 ] \
   || [ "$ERROR_XARGS_RC" -ne 0 ] \
   || [ "$RESULT_COUNT" -lt "$DISPATCHED" ]; then
    echo "ABORTED: run did NOT complete -- $RESULT_COUNT of $DISPATCHED fixtures reported."
    echo "  (xargs rc: happy=$HAPPY_XARGS_RC error=$ERROR_XARGS_RC, interrupted=$_INTERRUPTED)"
    echo "  partial tally so far: $PASS passed, $FAIL failed -- THIS IS NOT A PASS."
    if [ $FAIL -ne 0 ]; then
        for f in "${FAILED[@]}"; do echo "  - $f"; done
    fi
    # 2 = incomplete/aborted; distinct from 0 (all passed) and 1 (test failures).
    exit 2
fi

echo "summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
