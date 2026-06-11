#!/usr/bin/env bash
# tests/run-turi.sh -- interpreter (turi) fixture runner.
#
# Runs a curated subset of tests/fixtures/ through `tur run` (the tree-walk
# interpreter) instead of compiling each to a native binary.  Fixtures that
# require compilation (requires.compiled), or whose features are not yet
# complete in the interpreter, are skipped.
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
stamp_key() { echo "$(_tur_hash_file "$1")-$(_tur_mtime "$TUR")"; }
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
# Default fixture include-list: fixtures whose features the interpreter
# handles correctly.  Add new entries here as interpreter coverage grows.
# Format: one fixture name per line (matched as exact prefix or full name).
# ---------------------------------------------------------------------------
TURI_FIXTURES_DEFAULT="
adt-basic
adt-copy
adt-nested
adt-param
adt-param-match-type
adt-param-tyvar
adt-recursive
affine-basic
affine-drop
affine-fn-param
any-type-basic
arith
block-comment
borrow-basic
borrow-closure
borrow-defer
borrow-deref
borrow-mut-assign
borrow-reborrow
borrow-struct-field
borrow-sugar
call-cc-star
clone-primitives
closure-call
clone-list
clone-option
clone-pair
closure-multi-capture
closure-multi-capture-ref
continuation-advanced
continuation-basic
continuation-callcc
continuation-escape
continuation-escape-fn
defer-conditional
defer-early-return
defer-in-loop
defer-mutated-binding
defer-nested-scopes
defer-order
defstruct-copy-valid
defstruct-move-annotation
dynvar-binding
dynvar-convey
dynvar-convey-isolation
dynvar-inject
dynvar-log-level
dynvar-multi
dynvar-nested
dynvar-read
dynvar-set
effect-abort
effect-capture-k
effect-console
effect-cont-abort
effect-cont-linear
effect-cont-pred
effect-declaration
effect-deep-handler
effect-defer
effect-handler
effect-handler-compose
effect-handler-shadow
effect-hierarchy
effect-if-union
effect-log
effect-multiple
effect-nested
effect-oneshot
effect-perform-handle
effect-resume-value
effect-syntax
effect-syntax-compat
effect-with-fail
effect-with-write
gadt-adt-skolem
gadt-guard
gadt-param-tyvar
gadt-refine-basic
gadt-refine-expr
gadt-syntax-basic
gadt-syntax-multi
kind-inference-adt
let-star
match-literal
match-redundant-arm
panic-basic
panic-defer
panic-double-panic
panic-downcast
panic-ref
panic-trace
panic-with-typed
ptc4-basic
rc-basic
rc-ref-conversion
ref-basic
result-basic
typed/grid-basic
typed/list-basic
typed/list-macro
typed/map-basic
typed/map-collision
typed/map-eq
typed/option-basic
typed/pair-basic
typed/result-basic
typed/set-basic
typed/slice-basic
typed/vec-basic
typed/zipper-basic
union-types-basic
union-types-cast
union-types-match
union-types-threeway
unique-basic
weak-dangling
weak-upgrade

# TI0 (typeclass-correctness audit, 2026-06-10): fixtures verified to pass under
# turi after the poly-closure typed-dispatch (#293/#296/#297/#300), arrow
# carrier-class routing (#318), and HKT consolidation work landed.  Dispatch
# is shared via the elaborated Expr tree, so these were healthy from day one
# but were not on the allowlist.
arrow-compose-float
arrow-instance-apply
arrow-instance-arr-identity
arrow-instance-basic
arrow-instance-choice
arrow-instance-stdlib-basic
fat-shim-void-ptr-arrow-compose
hkt-stdlib-result-ok-biased
instance-head-hole-pair
poly-closure-compose-float
poly-closure-result-tyvar-float

# TI1 (turi-parity-post-v1-plan, quick wins): expression kinds newly handled
# by the interpreter.
#   letrec-basic     -- EX_LETREC: self- and mutual-recursion (TI1.1)
#   struct-set-field -- EX_SET_FIELD: in-place struct field write (TI1.4)
letrec-basic
struct-set-field

# TI2 (turi-parity-post-v1-plan, generators): EX_GEN / EX_YIELD / EX_GEN_NEXT /
# EX_GEN_DONE handled via fiber-backed coroutines, plus gen-some?/gen-unwrap/
# gen-none native overrides for stdlib/gen.tur's inline-C helpers.  Fixtures
# below drive generators through the stdlib helper path (no user inline-C).
#   gen-basic       -- finite generator summed via a manual gen-next loop
#   gen-done        -- gen-done? flips only after the body runs off its end
#   gen-nested-turi -- an outer generator driving an inner one (two live coros)
#   gen-for-each    -- gen-for-each macro over a finite generator
#   gen-nth         -- gen-nth macro: nth yielded value
#   gen-yield-star  -- yield* macro: re-yield an inner generator
gen-basic
gen-done
gen-nested-turi
gen-for-each
gen-nth
gen-yield-star

# Bugfix (docs/reported/turi-inline-c-ignores-comparison-operator.md): the
# interpreter's simple inline-C evaluator now honours trailing binary operators
# in `return <expr>;` instead of silently dropping them.
#   inline-c-binop -- != / == / > / + / * / % / << / && / ! shapes match tur
inline-c-binop

# TI3 (turi-parity-post-v1-plan, delimited control): abortive reset/shift/shift0
# plus serial-reset/cloneable-reset prompt boundaries handled by the interpreter
# (EX_RESET/EX_SHIFT/EX_SHIFT0/EX_SERIAL_RESET/EX_CLONEABLE_RESET).  Verified
# under 'tur --interpret'.  The context-capturing serial-shift/cloneable-shift
# remain a documented carve-out
# (docs/reported/turi-capturing-shift-unimplemented.md).
#   continuation-substrate -- abortive reset/shift/shift0 incl. nested resets
#   shift-result-typing    -- (shift f body) yields f's codomain
#   shift0-result-typing   -- shift0 mirrors shift's local typing/abort
#   serial-reset-basic     -- serial-reset with no shift returns its body
continuation-substrate
shift-result-typing
shift0-result-typing
serial-reset-basic

# TI6 (turi-parity-post-v1-plan, first-class handlers): the interpreter now
# handles EX_HANDLER_LIT, EX_WITH_HANDLER, and EX_COMPOSE_HANDLERS by reusing
# the eval_handle fiber machinery (a handler value is a detached HandleCase
# table; with-handler materialises a HandleExpr and runs it like (handle ...)).
# Verified under 'tur --interpret'.  EX_SELECT stays a carve-out -- channels
# need native primitives the interpreter lacks, and every select fixture is
# inline-C-bound (docs/reported/turi-select-needs-channel-primitives.md).
#   with-handler-value  -- single handler value applied via with-handler
#   fh-compose-handlers -- compose-handlers over disjoint effects + with-handler
with-handler-value
fh-compose-handlers

# TI5 (turi-parity-post-v1-plan, panic payloads): the interpreter now carries a
# typed panic payload (TypeKind + value + file/line) across the catch boundary,
# so catch-panic-of filters by payload type and re-raises on mismatch, and the
# panic-payload-* accessors read the caught value.  Verified under
# 'tur --interpret'.  The accessors are only reachable via the inline-C
# `result-panic` extractor (a carve-out), so coverage is the catch-panic-of
# type-filtering path.
#   panic-catch-panic-of -- plain (cstr) panic: :cstr matches, :int re-raises
#   panic-with-catch-of  -- typed panic-with payload: :int matches, :cstr re-raises
panic-catch-panic-of
panic-with-catch-of
"

# Build an associative-set from the default list for O(1) lookup.
# We use a naming convention: TURI_INCL_<name>=1
while IFS= read -r _fixture; do
    _fixture="${_fixture#"${_fixture%%[![:space:]]*}"}"  # ltrim
    _fixture="${_fixture%"${_fixture##*[![:space:]]}"}"  # rtrim
    [ -z "$_fixture" ] && continue
    case "$_fixture" in \#*) continue ;; esac
    # sanitize name for use as shell variable: replace - and / with _
    _key="$(printf '%s' "$_fixture" | tr '-' '_' | tr '/' '_')"
    eval "export TURI_INCL_${_key}=1"
done <<< "$TURI_FIXTURES_DEFAULT"

fixture_in_turi_set() {
    local name="$1"
    local key
    key="$(printf '%s' "$name" | tr '-' '_' | tr '/' '_')"
    eval "[ \"\${TURI_INCL_${key}:-0}\" = \"1\" ]"
}

run_turi_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input

    # Skip if not in the turi include set.  Emit a visible SKIP so allowlist
    # gaps don't go unnoticed -- see KB-001 in docs/archive/history/known-bugs.md.
    if ! fixture_in_turi_set "$name"; then
        printf 'SKIP %s (not in turi allowlist)\n' "$name"
        echo "SKIP_ALLOWLIST" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    # Skip fixtures that explicitly require compiled execution.
    if [ -f "$dir/requires.compiled" ]; then
        printf 'SKIP %s (requires.compiled)\n' "$name"
        return
    fi

    # Skip fixtures explicitly marked as not interpretable under turi
    # (TI1 turi-parity-post-v1-plan): a feature the tree-walking interpreter
    # deliberately does not implement (mirror of requires.compiled, but keyed
    # on interpreter capability rather than codegen need).  When the harness
    # flips from allowlist to denylist (TI8) this marker is what keeps such a
    # fixture out of the turi run.
    if [ -f "$dir/requires.tur-only" ]; then
        printf 'SKIP %s (requires.tur-only)\n' "$name"
        return
    fi

    # Locate input file.
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else printf 'SKIP %s (no input)\n' "$name"; return; fi

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
            timeout "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags run "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    else
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags run "$input" \
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

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE
export -f run_turi_fixture fixture_in_turi_set stamp_check stamp_write stamp_key
export -f _tur_hash_file _tur_mtime

# Build list of all fixture dirs (top-level and one subdirectory deep).
shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
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

# Tally results.
ALLOWLIST_GAP=0
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
    elif [ "$kind" = "SKIP_ALLOWLIST" ]; then
        SKIP=$((SKIP + 1))
        ALLOWLIST_GAP=$((ALLOWLIST_GAP + 1))
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
if [ "$ALLOWLIST_GAP" -gt 0 ]; then
    echo "  (of which $ALLOWLIST_GAP not in turi allowlist; see KB-001)"
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
