#!/usr/bin/env bash
# tests/run-r7rs-gc.sh -- the r7rs-gc experiment (docs/upcoming/r7rs-gc-plan.md).
#
#   1. Every `#lang r7rs` fixture, built with --enable=r7rs-gc and run with
#      TUR_GC_TORTURE (a collection every N allocations; default 31), must still
#      print its expected.stdout and exit as expected.  A conservative
#      collector's one real failure is a MISSING ROOT -- memory it cannot see
#      holding the only pointer to an object -- and collecting that often turns
#      one into a crash or a wrong answer instead of a rare heisenbug.
#   2. Reclamation: a loop that builds and drops a million small lists runs
#      under a 256 MiB address-space limit.  With the collector it fits; the
#      same program without it (429 MB peak, docs/reported/
#      r7rs-heap-data-never-reclaimed.md) must NOT fit, or the check proves
#      nothing and fails.
#
# Linux/glibc only (the collector is; elsewhere it is plain malloc).
#   R7RS_GC_TORTURE=N   the torture interval (default 31, about two minutes on
#                       four cores; 1 collects on EVERY allocation, the deep run
#                       to make before touching the collector or its roots).

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "r7rs-gc: $TUR not built" >&2; exit 2; }
if [ "$(uname -s)" != "Linux" ]; then
    echo "r7rs-gc: SKIP (host is $(uname -s))"
    exit 0
fi
TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
TORTURE="${R7RS_GC_TORTURE:-31}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fixtures=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ -f "$d/input.tur" ] && [ -f "$d/expected.stdout" ] || continue
    case "$(basename "$d")" in
        r7rs-*) fixtures+=("$d") ;;
        *) IFS= read -r first < "$d/input.tur"
           [[ "$first" == "#lang r7rs"* ]] && fixtures+=("$d") ;;
    esac
done

one() {
    local name; name="$(basename "$1")"
    one_case "$1" > "$WORK/$name.result" 2>&1
}
one_case() {
    local dir="$1" name rc want flags args=()
    name="$(basename "$dir")"
    flags=""; [ -f "$dir/flags" ] && flags="$(cat "$dir/flags")"
    [ -f "$dir/run.args" ] && mapfile -t args < "$dir/run.args"
    local stdin=/dev/null; [ -f "$dir/input.stdin" ] && stdin="$dir/input.stdin"
    # shellcheck disable=SC2086
    if ! timeout 600 "$TUR" $flags --enable=r7rs-gc build "$dir/input.tur" \
            -o "$WORK/$name" > "$WORK/$name.build" 2>&1; then
        echo "FAIL $name -- build failed: $(grep -m1 -i error "$WORK/$name.build" | cut -c1-160)"
        return
    fi
    (cd "$dir" && ASAN_OPTIONS=detect_leaks=0 TUR_GC_TORTURE="$TORTURE" \
        timeout 300 "$WORK/$name" "${args[@]}" < "$stdin" \
        > "$WORK/$name.out" 2> "$WORK/$name.err") 2> /dev/null
    rc=$?
    want=0; [ -f "$dir/expected.exit" ] && want="$(tr -d '[:space:]' < "$dir/expected.exit")"
    if [ "$rc" = 124 ]; then
        echo "FAIL $name -- timed out (>300s) under TUR_GC_TORTURE=$TORTURE"
    elif { [ "$want" = nonzero ] && [ "$rc" = 0 ]; } || { [ "$want" != nonzero ] && [ "$rc" != "$want" ]; }; then
        echo "FAIL $name -- exit $rc, expected $want: $(tail -1 "$WORK/$name.err" | cut -c1-120)"
    elif ! diff -q "$WORK/$name.out" "$dir/expected.stdout" > /dev/null; then
        echo "FAIL $name -- stdout differs with the collector"
        diff "$WORK/$name.out" "$dir/expected.stdout" | head -6 | sed 's/^/    /'
    else
        echo "PASS $name"
    fi
}
export -f one one_case
export TUR WORK TORTURE

printf '%s\n' "${fixtures[@]}" | xargs -P "$(nproc)" -I{} bash -c 'one "$@"' _ {}
for d in "${fixtures[@]}"; do cat "$WORK/$(basename "$d").result"; done | tee "$WORK/results"

# 2. Reclamation under an address-space limit.
cat > "$WORK/churn.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write))
(define (churn i)
  (if (= i 0) 'done
      (begin (list i i i i) (churn (- i 1)))))
(write (churn 1000000))
(newline)
EOF
reclaim() {
    local tag="$1"; shift
    if ! "$TUR" "$@" build "$WORK/churn.tur" -o "$WORK/churn-$tag" > "$WORK/churn-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    (ulimit -v 262144; "$WORK/churn-$tag" 2>/dev/null) 2>/dev/null || true
}
with="$(reclaim gc --enable=r7rs-gc)"
without="$(reclaim plain)"
if [ "$with" != "done" ]; then
    echo "FAIL reclaim -- with the collector the churn did not fit in 256 MiB (got '$with')"
elif [ "$without" = "done" ]; then
    echo "FAIL reclaim -- the churn fits in 256 MiB WITHOUT the collector, so this check bites on nothing"
else
    echo "PASS reclaim (256 MiB: fits with the collector, not without)"
fi | tee -a "$WORK/results"

pass=$(grep -c '^PASS' "$WORK/results")
fail=$(grep -c '^FAIL' "$WORK/results")
echo
echo "r7rs-gc: $pass passed, $fail failed (torture every $TORTURE allocations)"
[ "$fail" -eq 0 ]
