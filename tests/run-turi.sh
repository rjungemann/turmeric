#!/usr/bin/env bash
# tests/run-turi.sh -- interpreter (turi) fixture runner.
#
# Runs EVERY tests/fixtures/ through `tur --interpret` (the tree-walking turi
# interpreter, src/turi/eval.c) instead of compiling each to a native binary.
# Fixtures that require compilation (requires.compiled), whose features the
# interpreter deliberately does not implement (requires.tur-only), or whose
# body contains a user inline-C (```c) block (a permanent TI7 carve-out) are
# PASS-skipped; see the W5-flip block further down for the full skip rules.
#
# TI8 history (turi-parity-post-v1-plan): this harness used to invoke `tur run`,
# which COMPILES and runs a native binary -- so an earlier allowlist never
# actually exercised src/turi/eval.c (the now-resolved blocker report
# docs/archive/history/turi-harness-compiles-instead-of-interpreting.md).  It now
# uses `--interpret`, and the full allowlist -> denylist flip (TI8.b/W5) has
# LANDED: the hand-curated TURI_FIXTURES_DEFAULT is gone and the harness defaults
# to run-everything-minus-markers.  The record of how the ~933-fixture gap was
# driven to zero lives in
# docs/archive/history/turi-harness-flip-reconciliation.md and
# docs/archive/history/turi-interpreter-gap-closure-plan.md.
#
# Usage:
#   bash tests/run-turi.sh                  # run the default turi fixture set
#   TURI_FILTER='borrow' bash tests/run-turi.sh   # run only matching fixtures
#
# Environment:
#   TUR              path to the tur binary (default: ./build/tur)
#   TURI_FILTER      optional additional grep -E pattern to narrow the run
#                    (TUR_TEST_FILTER is accepted as an alias for parity with
#                    tests/run.sh -- see KB-002 in docs/archive/history/known-bugs.md)
#   TUR_TEST_JOBS    parallelism (default: cpu count, capped at 8)
#   TUR_FORCE        set to 1 to skip stamp-cache fast-path

set -u
cd "$(dirname "$0")/.."

# This harness runs every fixture through the tree-walking turi/eval
# interpreter, which intentionally never frees its closures or registered
# natives (they live for the process lifetime). LeakSanitizer (enabled with
# ASan on Linux) would otherwise flag that design choice as a leak. Default to
# detect_leaks=0 to mirror the ctest policy (turi_fixture_tests); opt back in
# with ASAN_OPTIONS=detect_leaks=1 bash tests/run-turi.sh. See
# docs/asan-debug-leaks-plan.md.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "run-turi: $TUR not built; run 'just build' first" >&2; exit 2; }

PASS=0
FAIL=0
SKIP=0
FAILED=()

if command -v getconf >/dev/null 2>&1; then
    _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
elif command -v sysctl >/dev/null 2>&1; then
    _nproc="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
    _nproc=4
fi
JOBS="${TUR_TEST_JOBS:-$_nproc}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

TUR_FORCE="${TUR_FORCE:-0}"
STAMP_CACHE="tests/.stamp-cache-turi"

_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }

# Performance optimization: cache the compiler binary modification time once
# at startup so we do not spawn a redundant stat process for every single fixture.
export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}"; }
stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    [ -f "$sf" ] && [ "$(cat "$sf")" = "$(stamp_key "$2")" ]
}
stamp_write() {
    mkdir -p "$STAMP_CACHE"
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    stamp_key "$2" > "$sf"
}

RESULTS_DIR="$(mktemp -d -t turi-tests-results-XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# ---------------------------------------------------------------------------
# W5 flip (turi-interpret-flip-residual / turi-interpreter-gap-closure-plan):
# this harness no longer carries an allowlist.  It runs EVERY tests/fixtures/*
# through `tur --interpret`, minus (a) the requires.{compiled,tur-only,
# dedicated-runner,spices,tsan} marker skips and (b) fixtures whose body
# contains a user inline-C (```c) block -- a permanent TI7 carve-out the
# tree-walking interpreter cannot run (auto-detected, no per-fixture marker).
# tests/fixtures/errors/* are negative fixtures handled by the dedicated
# error-diag pass below, not this positive pass.  The historical allowlist
# (TURI_FIXTURES_DEFAULT) and its O(1) lookup set are gone; the record of how
# the gap was driven to zero lives in
# docs/archive/history/turi-harness-flip-reconciliation.md.
# ---------------------------------------------------------------------------

# TI8.b/W2: true if the fixture body contains a user inline-C (```c) block -- a
# permanent TI7 carve-out the interpreter does not run.
fixture_has_inline_c() {
    local dir="$1"
    local f
    if   [ -f "$dir/input.tur" ]; then f="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then f="$dir/$(basename "$dir").tur"
    else return 1; fi
    grep -q '```c' "$f" 2>/dev/null && return 0
    # The carve-out is about whether the PROGRAM contains inline-C, not whether
    # that inline-C is spelled in the fixture file.  A fixture that reaches it
    # through `(load "stdlib/<mod>.tur")` -- e.g. the rc/weak fixtures, whose
    # inline-C lives in the loaded module -- is just as unrunnable under the
    # tree-walking interpreter, but the own-file grep above says otherwise and
    # the fixture then runs and fails on the first unsupported call.  Follow one
    # level of `load`, which is all any fixture uses.
    local loaded
    while IFS= read -r loaded; do
        [ -n "$loaded" ] || continue
        [ -f "$loaded" ] || continue
        grep -q '```c' "$loaded" 2>/dev/null && return 0
    done < <(sed -n 's/.*(load "\([^"]*\)").*/\1/p' "$f" 2>/dev/null)
    return 1
}

# W5 flip: a small set of fixtures carry a ```c block yet DO run correctly under
# --interpret, because their inline-C ops are backed by native overrides
# (wk_register_* in src/main.c) or handled by the simple inline-C evaluator
# (try_exec_simple_inline_c in src/turi/eval.c).  The mechanical inline-C carve
# would skip these and silently drop genuine interpreter coverage, so they are
# kept explicitly and run (checked before the carve).  Keep this list tight: an
# entry belongs here only if it is verified to interpret correctly.
TURI_INLINEC_RUN="
inline-c-binop
map-multiword-struct-key
set-multiword-struct-element
clone-list
clone-option
clone-pair
generic-relay-aggregate-result
tuplen-struct-param-passing
typed/slice-basic
contract-ffi
seq-core-from-list
seq-transform-filter-map
hamt-lowering-basic
"
while IFS= read -r _fx; do
    _fx="${_fx#"${_fx%%[![:space:]]*}"}"; _fx="${_fx%"${_fx##*[![:space:]]}"}"
    [ -z "$_fx" ] && continue
    case "$_fx" in \#*) continue ;; esac
    eval "export TURI_ICRUN_$(printf '%s' "$_fx" | tr '-' '_' | tr '/' '_')=1"
done <<< "$TURI_INLINEC_RUN"

fixture_inline_c_runs() {
    local key; key="$(printf '%s' "$1" | tr '-' '_' | tr '/' '_')"
    eval "[ \"\${TURI_ICRUN_${key}:-0}\" = \"1\" ]"
}

# ---------------------------------------------------------------------------
# TI8.b/W3 (turi-interpreter-gap-closure-plan): error-fixture coverage under the
# interpreter.  tests/fixtures/errors/* are negative fixtures that must elaborate
# to a specific diagnostic (expected.diag).  run.sh validates them on the
# compiled path; this harness now runs them under `tur --interpret` too and does
# the same substring diag comparison.  All but the few below already emit the
# identical diagnostic (the interpreter shares the elaborator, so move/linearity/
# affine checks etc. match by construction).  The 3 "reporting-stage" divergences
# (unbound-call-head, unknown-helper-load-hint, tce3-map-heterogeneous-val) were
# fixed: an unbound runtime-dispatch call head now reports the compiler's
# "unknown function or operator" diagnostic (+ load-hint), and cmd_eval prints a
# runtime error from main instead of swallowing it.  The serial-shift
# capturability cases (serial-context-{,do-}not-capturable) now emit TUR-E0706
# under --interpret too: ts_capture_and_run rejects an uncapturable context with
# the same diagnostic the compiled path raises.
# This closes the TI0-noted gap that errors/ was skipped wholesale.
#
# Remaining reporting-stage divergence (interpreter emits a strict subset, not a
# wrong diagnostic):
#   defdata-malformed-ctor-field-type -- the compiled batch elaborator reports
#     BOTH the malformed-field-type error AND the follow-on "unknown function or
#     operator 'MkAcc'" at the later constructor reference; the interpreter halts
#     at the first hard elaboration error and never reaches the MkAcc call, so it
#     emits only the first line.  The first (primary) diagnostic matches; only
#     the compiled path's second, cascade diagnostic is missing.
# ---------------------------------------------------------------------------
TURI_ERRORS_DENY="
defdata-malformed-ctor-field-type
"
while IFS= read -r _ef; do
    _ef="${_ef%%#*}"                                   # strip trailing comment
    _ef="${_ef#"${_ef%%[![:space:]]*}"}"; _ef="${_ef%"${_ef##*[![:space:]]}"}"
    [ -z "$_ef" ] && continue
    _ek="$(printf '%s' "$_ef" | tr '-' '_')"
    eval "export TURI_ERRDENY_${_ek}=1"
done <<< "$TURI_ERRORS_DENY"

err_in_denyset() {
    local key; key="$(printf '%s' "$1" | tr '-' '_')"
    eval "[ \"\${TURI_ERRDENY_${key}:-0}\" = \"1\" ]"
}

# Run a single tests/fixtures/errors/<name> fixture under --interpret and verify
# every expected.diag line appears (substring) in the interpreter's stderr.
run_turi_error_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"           # e.g. errors/linear-dropped
    local base="${name#errors/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"

    [ -f "$dir/input.tur" ] || return
    # Only diag-style negative fixtures (must have a non-empty expected.diag).
    [ -s "$dir/expected.diag" ] || return
    if [ -f "$dir/requires.compiled" ] || [ -f "$dir/requires.tur-only" ] \
       || [ -f "$dir/requires.spices" ]; then return; fi
    if err_in_denyset "$base"; then
        printf 'SKIP %s (errors denylist: interp diag diverges)\n' "$name"
        return
    fi

    if stamp_check "$name" "$dir/input.tur"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
    local err="$dir/turi.stderr"
    if command -v timeout >/dev/null 2>&1; then
        timeout 15 "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    else
        "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    fi

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
        echo "FAIL $name -- interpreter diagnostic mismatch"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
    fi
}

run_turi_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input

    # Locate input first.  A directory with no input.tur / <name>.tur is a
    # container (e.g. tests/fixtures/typed/), not a fixture -- skip it silently.
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi

    # Marker skips.  Mirror of run.sh's skip set:
    #   requires.compiled       -- needs the compiled path
    #   requires.tur-only       -- a feature the interpreter deliberately omits
    #   requires.dedicated-runner -- owned by its own ctest target
    #   requires.spices         -- needs the sibling ../turmeric-spices checkout
    #   requires.tsan           -- TSan-only fixture
    if [ -f "$dir/requires.compiled" ]; then
        printf 'SKIP %s (requires.compiled)\n' "$name"; return; fi
    if [ -f "$dir/requires.tur-only" ]; then
        printf 'SKIP %s (requires.tur-only)\n' "$name"; return; fi
    if [ -f "$dir/requires.dedicated-runner" ]; then
        printf 'SKIP %s (requires.dedicated-runner)\n' "$name"; return; fi
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        printf 'SKIP %s (requires.spices; sibling checkout absent)\n' "$name"; return; fi
    if [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ]; then
        printf 'SKIP %s (requires.tsan)\n' "$name"; return; fi

    # W5 flip (turi-interpreter-gap-closure-plan): the only positive-fixture skip
    # left is the *permanent* inline-C carve-out.  User inline-C (a ```c block) is
    # a TI7 carve-out the tree-walking interpreter never runs, auto-detected here
    # so the ~400 inline-C fixtures are skipped without per-fixture markers.  The
    # inline-C fixtures that DO work under turi (via try_exec_simple_inline_c or
    # native overrides, e.g. inline-c-binop / gen-*) are run because their bodies
    # carry the working ops through a native shim rather than a raw ```c block --
    # those few keep a presence in the suite via the dedicated paths.  Note: some
    # inline-C fixtures silently miscompile under the simple inline-C evaluator --
    # a real bug the carve hides; see
    # docs/reported/turi-inline-c-silent-miscompiles.md.  Every other fixture is
    # now run for real under --interpret (no allowlist gate).
    if fixture_has_inline_c "$dir" && ! fixture_inline_c_runs "$name"; then
        printf 'SKIP %s (inline-c carve-out)\n' "$name"
        echo "SKIP_INLINEC" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    # Stamp fast-path.
    if stamp_check "$name" "$input"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    local actual_stdout="$dir/turi.stdout"
    local actual_stderr="$dir/turi.stderr"

    # Fixture-specific flags.
    local fixture_flags=""
    [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")

    # Per-fixture timeout (default 15 s for interpreter -- slightly longer than compiled).
    local fixture_timeout=15
    if [ -f "$dir/expected.timeout" ]; then
        local _t; _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    # Run via interpreter.
    local rc=0
    if [ -f "$dir/input.stdin" ]; then
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    else
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    fi

    # Expected exit code.
    local expected_exit="0"
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")

    # Check stdout.
    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null 2>&1; then
            echo "FAIL $name -- stdout mismatch"
            diff -u "$dir/expected.stdout" "$actual_stdout" | head -20 | sed 's/^/    /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    # Check exit code.
    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "FAIL $name -- expected nonzero exit, got 0"
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL $name -- exited $rc (expected $expected_exit)"
            [ -s "$actual_stderr" ] && head -5 "$actual_stderr" | sed 's/^/    stderr: /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    stamp_write "$name" "$input"
    echo "PASS $name"
    echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
}

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE TUR_MTIME
export -f run_turi_fixture fixture_inline_c_runs fixture_has_inline_c stamp_check stamp_write stamp_key
export -f _tur_hash_file _tur_mtime
export -f run_turi_error_fixture err_in_denyset

# Build list of all fixture dirs (top-level and one subdirectory deep).
# tests/fixtures/errors/* are negative fixtures handled by the dedicated
# error-diag pass below, so they are excluded from this positive pass.
shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    case "$d" in tests/fixtures/errors|tests/fixtures/errors/*) continue ;; esac
    [ -d "$d" ] || continue
    ALL_DIRS+=("$d")
done

# Apply optional additional filter.  Accept TUR_TEST_FILTER as an alias so the
# same filter env var works against both tests/run.sh and tests/run-turi.sh.
# TURI_FILTER wins when both are set.
TURI_FILTER="${TURI_FILTER:-${TUR_TEST_FILTER:-}}"
FILTERED_DIRS=()
for d in "${ALL_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; then
        FILTERED_DIRS+=("$d")
    fi
done

if [ ${#FILTERED_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${FILTERED_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_fixture "$@"' _ {} 2>/dev/null
fi

# TI8.b/W3: error-fixture diag pass (tests/fixtures/errors/*).  Honors the same
# TURI_FILTER so `TURI_FILTER=errors/ ...` narrows to just this pass.
ERROR_DIRS=()
for d in tests/fixtures/errors/*/; do
    d="${d%/}"; [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; then
        ERROR_DIRS+=("$d")
    fi
done
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${ERROR_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_error_fixture "$@"' _ {} 2>/dev/null
fi

# Tally results.
INLINEC_CARVE=0
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
    elif [ "$kind" = "SKIP_INLINEC" ]; then
        SKIP=$((SKIP + 1))
        INLINEC_CARVE=$((INLINEC_CARVE + 1))
    fi
done

# §4.2: Run REPL-based async eval test scripts.
for _async_sh in tests/turi/eval-async-*.sh; do
    [ -x "$_async_sh" ] || continue
    _async_name="$(basename "${_async_sh%.sh}")"
    if TUR="$TUR" bash "$_async_sh" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
        printf 'PASS %s\n' "$_async_name"
    else
        FAIL=$((FAIL + 1))
        FAILED+=("$_async_name")
        printf 'FAIL %s\n' "$_async_name"
    fi
done

echo
echo "turi fixture summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$INLINEC_CARVE" -gt 0 ]; then
    echo "  (of which $INLINEC_CARVE inline-c carve-outs -- TI7, never run under turi)"
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
