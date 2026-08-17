#!/usr/bin/env bash
# tests/run-jit.sh -- MIR JIT fixture runner (J3, jit-engine-plan section 5).
#
# Runs EVERY tests/fixtures/ through `tur --enable=jit jit` -- the in-process
# MIR engine (src/jit_engine.c) -- instead of compiling each to a native
# binary through cc.  The engine's own step-6 fallback to the cc path is a
# first-class outcome here: a fixture whose inline C c2mir rejects still runs
# and still must produce the expected output; it is tallied separately
# ("via cc fallback") as a signal, never a failure.
#
# Result semantics mirror tests/run.sh's run phase (expected.stdout diff +
# expected.exit, run.args, input.stdin, expected.timeout), NOT the sweep's
# (tools/jit-spike/sweep-turjit.sh scores any signal as FAIL; this harness
# honors expected.exit, so by-design-panic fixtures pass here).
#
# Usage:
#   bash tests/run-jit.sh                       # the full fixture set
#   TUR_TEST_FILTER='hamt' bash tests/run-jit.sh  # narrow by regex
#
# Environment:
#   TUR            path to a TUR_JIT=ON tur (default: ./build-turjit/tur,
#                  falling back to ./build/tur).  A binary without the
#                  engine SKIPs the whole run (exit 0) so this harness is
#                  safe to invoke against any build.
#   TUR_TEST_JOBS  parallelism (default: cpu count, capped at 8)
#   TUR_FORCE      set to 1 to skip the stamp-cache fast-path
#
# Markers (mirroring run-turi.sh's posture):
#   requires.cc               -- genuinely cc-only under the JIT; PASS-skip
#   requires.interp           -- interpreter-owned fixture; PASS-skip
#   requires.dedicated-runner -- owned by its own ctest target; PASS-skip
#   requires.spices           -- skipped when the sibling checkout is absent
#   requires.tsan             -- skipped unless TUR_TSAN=1
#
# Like tests/run.sh, a fully green run needs a DEBUG-configured tur: the
# refine-* fixtures depend on Debug-only refinement discharge and fail on
# any Release binary, jit or not (the known "refine-* on a Release tur"
# class, findings 21.3). Expect green against build-turjit-debug/tur and
# exactly the refine set red against a Release build.

set -u
cd "$(dirname "$0")/.."

# Plan section 6: JIT harness runs mirror the interpreter harness posture --
# the program runs INSIDE the (possibly ASan) tur process, whose engine
# deliberately leaks its context on unwound errors and whose runtime keeps
# process-lifetime registrations, so LeakSanitizer noise is by design.  Opt
# back in with ASAN_OPTIONS=detect_leaks=1.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# Force server fixtures to bind 127.0.0.1 instead of INADDR_ANY -- the same
# export tests/run.sh:72 makes, and it must match: the stdlib listen path
# (stdlib/httpd.tur:703) reads this env at RUN time, so a fixture behaves
# differently under a harness that omits it.
#
# httpd-new-pool-fail-drops-handler is the fixture that caught the omission.  It
# occupies 127.0.0.1:<port> and then asserts httpd-new-pool's bind of the same
# port is refused.  Without this export httpd binds 0.0.0.0 instead, and BSD's
# SO_REUSEADDR (both sockets set it) permits a wildcard bind while a SPECIFIC
# address holds the port -- so the bind SUCCEEDED and the fixture printed
# "built" instead of "refused".  Linux refuses that bind either way, which is
# why this only ever showed up on macOS and read as a JIT/BSD defect.  It is
# neither: it was harness drift from run.sh.
export TUR_BIND_LOOPBACK=1

TUR="${TUR:-./build-turjit/tur}"
[ -x "$TUR" ] || TUR=./build/tur
[ -x "$TUR" ] || { echo "run-jit: no tur binary found" >&2; exit 2; }

# Capability probe -- capture, don't pipe into grep -q (pipefail SIGPIPE).
# Probed with a nonexistent input: P0 (engine-selection-plan) moved cmd_jit's
# input scan ahead of its gates, so a bare `tur jit` prints usage on EVERY
# build and no longer discriminates.  A non-JIT binary answers "carries no
# JIT engine" before touching the file; a JIT binary proceeds to (and fails)
# the compile.
probe=$("$TUR" --enable=jit jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "run-jit: SKIP ($TUR carries no JIT engine; configure -DTUR_JIT=ON)"
     exit 0 ;;
esac

# The engine's cc fallback links -lturi; anchor -L at the build tree the
# binary actually lives in, exactly as tests/run.sh does.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

PASS=0
FAIL=0
SKIP=0
FALLBACK=0
FAILED=()

if command -v getconf >/dev/null 2>&1; then
    _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
else
    _nproc=4
fi
JOBS="${TUR_TEST_JOBS:-$_nproc}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

TUR_FORCE="${TUR_FORCE:-0}"
STAMP_CACHE="tests/.stamp-cache-jit"

_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }

# Stock macOS ships no `timeout(1)` -- Homebrew coreutils installs it as
# `gtimeout` unless the gnubin path is on PATH.  run.sh has detected this since
# T19; this harness called bare `timeout`, so on a Mac without GNU coreutils
# every fixture would have died in the runner rather than the compiler.  Mirror
# run.sh: prefer timeout, fall back to gtimeout, and run untimed if neither
# exists (a hung fixture then hangs the run, which is the pre-existing tradeoff
# run.sh already makes).
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then _tur_timeout_bin="gtimeout"; fi
_run_timed() {
    local secs="$1"; shift
    if [ "$secs" -le 0 ] || [ -z "$_tur_timeout_bin" ]; then "$@"
    else "$_tur_timeout_bin" "$secs" "$@"; fi
}

export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}"; }

stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local f="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__')"
    [ -f "$f" ] || return 1
    [ "$(cat "$f")" = "$(stamp_key "$2")" ]
}

stamp_write() {
    mkdir -p "$STAMP_CACHE"
    echo "$(stamp_key "$2")" > "$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__')"
}

RESULTS_DIR="$(mktemp -d -t tur-jit-results.XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# Known latent MISCOMPILES, discovered by this harness being the first to
# COMPILE the nested typed/* fixtures (run.sh scans only tests/fixtures/*/;
# these were interpreter-covered only).  Each failed identically under gcc
# and MIR -- the defect was in the emitted C, not an engine -- so they were
# denylisted here with their reports rather than failing every run.  Add an
# entry (with a report) if the class recurs; remove it when the report is
# resolved.
#
# Both original entries are now fixed and run normally: typed/result-basic by
# the cps->direct aggregate-carrier bridge (findings 28) and
# typed-slots/cs3-nested-specialization by the CS3 nested-spec result recovery
# (findings 30).  The list is deliberately kept (empty) -- it is the mechanism
# for carrying a compile-path miscompile without hiding it.
JIT_KNOWN_MISCOMPILE="
"
jit_known_miscompile() {
    local n
    for n in $JIT_KNOWN_MISCOMPILE; do
        [ "$1" = "$n" ] && return 0
    done
    return 1
}

run_jit_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"
    local input

    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi

    if [ -f "$dir/requires.cc" ]; then
        printf 'SKIP %s (requires.cc)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    # hook.sh fixtures delegate the whole build+run+normalize to a
    # fixture-owned script that invokes `tur build` itself -- the two-process
    # shape the JIT replaces.  They stay owned by run.sh.
    if [ -f "$dir/hook.sh" ]; then
        printf 'SKIP %s (hook.sh; owned by run.sh)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.interp" ] || [ -f "$dir/requires.interp-only" ]; then
        printf 'SKIP %s (interpreter-owned)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.dedicated-runner" ]; then
        printf 'SKIP %s (requires.dedicated-runner)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        printf 'SKIP %s (requires.spices; sibling checkout absent)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ]; then
        printf 'SKIP %s (requires.tsan)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if jit_known_miscompile "$name"; then
        printf 'SKIP %s (known miscompile -- see docs/reported/)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi

    if stamp_check "$name" "$input"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local fixture_flags=""
    [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")

    # Per-fixture timeout.  Default sits between run.sh's 10s (compiled) and
    # run-turi.sh's 15s: the program runs at native speed but pays the
    # engine's compile on top.
    local fixture_timeout=15
    if [ -f "$dir/expected.timeout" ]; then
        local _t; _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    local run_args_arr=()
    if [ -f "$dir/run.args" ]; then
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            run_args_arr+=("$_ra")
        done < "$dir/run.args"
    fi

    local actual_stdout="$dir/jit.stdout"
    local actual_stderr="$dir/jit.stderr"
    local stdin_file=/dev/null
    [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"

    local rc=0
    if [ "${#run_args_arr[@]}" -gt 0 ]; then
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags --enable=jit jit "$input" \
            -- "${run_args_arr[@]}" \
            < "$stdin_file" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
    else
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags --enable=jit jit "$input" \
            < "$stdin_file" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
    fi

    local fell_back=""
    grep -q 'TUR-W0070' "$actual_stderr" 2>/dev/null && fell_back=1

    local expected_exit="0"
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")

    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null 2>&1; then
            echo "FAIL $name -- stdout mismatch${fell_back:+ (via cc fallback)}"
            diff -u "$dir/expected.stdout" "$actual_stdout" | head -20 | sed 's/^/    /'
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    fi

    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "FAIL $name -- expected nonzero exit, got 0"
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL $name -- exited $rc (expected $expected_exit)${fell_back:+ (via cc fallback)}"
            [ -s "$actual_stderr" ] && tail -5 "$actual_stderr" | sed 's/^/    stderr: /'
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    fi

    if [ -n "$fell_back" ]; then
        # Correct output through the cc fallback: a pass, tallied separately
        # so the jit-native count stays an honest signal.  Not stamped -- a
        # future engine improvement should get the chance to reclaim it.
        printf 'PASS %s (via cc fallback)\n' "$name"
        echo "PASS_FALLBACK" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    stamp_write "$name" "$input"
    printf 'PASS %s\n' "$name"
    echo "PASS" > "$RESULTS_DIR/$rkey.result"
}

# Negative fixtures: the diagnostics come from the same front end as the cc
# path (compile_to_c fails before the engine is reached), so expected.diag
# must match under `tur jit` exactly as under `tur build`.
run_jit_error_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"

    [ -f "$dir/input.tur" ] || return
    [ -s "$dir/expected.diag" ] || return
    # requires.interp-only is checked alongside requires.interp, as the happy
    # path already does.  It was missing here, and the two markers are
    # near-homographs that do opposite things (see CLAUDE.md), so an `errors/`
    # fixture asserting an INTERPRETER diagnostic -- which is exactly what
    # requires.interp-only means for a negative fixture -- was run through
    # `tur jit` and reported `jit diagnostic mismatch` for a diagnostic no
    # compiled path ever emits.  Caught by
    # errors/turi-multishot-resume-past-fiber-body, the first such fixture.
    if [ -f "$dir/requires.cc" ] || [ -f "$dir/requires.interp" ] \
       || [ -f "$dir/requires.interp-only" ] \
       || [ -f "$dir/requires.spices" ]; then return; fi

    if stamp_check "$name" "$dir/input.tur"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
    local err="$dir/jit.stderr"
    # _run_timed, NOT bare `timeout` -- stock macOS ships no timeout(1) (see
    # the _tur_timeout_bin probe above).  This call site was missed when that
    # guard went in, and the failure is silent and total: `timeout` not found
    # makes the command fail, `|| true` swallows it, jit.stderr ends up with a
    # shell error instead of a diagnostic, and EVERY needle misses -- so all
    # ~400 negative fixtures report `jit diagnostic mismatch` as though the
    # compiler had stopped emitting diagnostics.  A Mac with Homebrew coreutils
    # on PATH does not reproduce it, which is why the local baseline was green
    # while macOS CI was not.
    _run_timed 15 "$TUR" $flags --enable=jit jit "$dir/input.tur" \
        >/dev/null 2>"$err" || true

    local missing=0 needle
    while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        grep -F -q "$needle" "$err" || missing=1
    done < "$dir/expected.diag"

    if [ "$missing" -eq 0 ]; then
        stamp_write "$name" "$dir/input.tur"
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
    else
        echo "FAIL $name -- jit diagnostic mismatch"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
    fi
}

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE TUR_MTIME TUR_CC_FLAGS
export JIT_KNOWN_MISCOMPILE
export -f run_jit_fixture run_jit_error_fixture jit_known_miscompile
export -f stamp_check stamp_write stamp_key _tur_hash_file _tur_mtime
export -f _run_timed
export _tur_timeout_bin

shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    case "$d" in tests/fixtures/errors|tests/fixtures/errors/*) continue ;; esac
    [ -d "$d" ] || continue
    ALL_DIRS+=("$d")
done

JIT_FILTER="${JIT_FILTER:-${TUR_TEST_FILTER:-}}"
FILTERED_DIRS=()
for d in "${ALL_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if [ -z "$JIT_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$JIT_FILTER"; then
        FILTERED_DIRS+=("$d")
    fi
done

if [ ${#FILTERED_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${FILTERED_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_jit_fixture "$@"' _ {} 2>/dev/null
fi

ERROR_DIRS=()
for d in tests/fixtures/errors/*/; do
    d="${d%/}"; [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if [ -z "$JIT_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$JIT_FILTER"; then
        ERROR_DIRS+=("$d")
    fi
done
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${ERROR_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_jit_error_fixture "$@"' _ {} 2>/dev/null
fi

for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    case "$kind" in
        PASS)          PASS=$((PASS + 1)) ;;
        PASS_FALLBACK) PASS=$((PASS + 1)); FALLBACK=$((FALLBACK + 1)) ;;
        FAIL)          FAIL=$((FAIL + 1)); FAILED+=("$name") ;;
        SKIP)          SKIP=$((SKIP + 1)) ;;
    esac
done

echo
echo "jit fixture summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$FALLBACK" -gt 0 ]; then
    echo "  (of which $FALLBACK passed via the cc fallback -- TUR-W0070)"
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
