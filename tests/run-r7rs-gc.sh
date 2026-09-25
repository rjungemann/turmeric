#!/usr/bin/env bash
# tests/run-r7rs-gc.sh -- the r7rs-gc collector (docs/archive/r7rs-gc-plan.md),
# on by default for a compiled `#lang r7rs` program since it graduated
# (2026-09-25); TUR_R7RS_GC=0 builds without it.
#
#   1. Every `#lang r7rs` fixture, built with the collector and run with
#      TUR_GC_TORTURE (a collection every N allocations; default 31), must still
#      print its expected.stdout and exit as expected.  A conservative
#      collector's one real failure is a MISSING ROOT -- memory it cannot see
#      holding the only pointer to an object -- and collecting that often turns
#      one into a crash or a wrong answer instead of a rare heisenbug.
#   2. The runtime archive's blocks: Scheme values kept only in a Turmeric
#      persistent map (a HAMT libturt_runtime.a allocates) survive a
#      collection on EVERY allocation.  Before the archive allocated through
#      the collector's hook (src/runtime/rt_alloc.h) this segfaulted: the
#      nodes were libc's, unscanned, and the values were freed under them.
#   3. Threads are refused: a program that starts one under the collector
#      exits 70 with a diagnostic naming the plan and the opt-out, and runs
#      under TUR_R7RS_GC=0.
#   4. Reclamation (Linux only, where `ulimit -v` binds): a loop that builds
#      and drops a million small lists runs under a 256 MiB address-space
#      limit.  With the collector it fits; the same program without it
#      (429 MB peak, docs/reported/r7rs-heap-data-never-reclaimed.md) must
#      NOT fit, or the check proves nothing and fails.
#
# Linux/glibc and macOS (the collector is; elsewhere it is plain malloc).
#   R7RS_GC_TORTURE=N   the torture interval (default 31, about two minutes on
#                       four cores; 1 collects on EVERY allocation, the deep run
#                       to make before touching the collector or its roots).

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "r7rs-gc: $TUR not built" >&2; exit 2; }
HOST="$(uname -s)"
if [ "$HOST" != "Linux" ] && [ "$HOST" != "Darwin" ]; then
    echo "r7rs-gc: SKIP (host is $HOST)"
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
    if ! timeout 600 "$TUR" $flags build "$dir/input.tur" \
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

# 2. The runtime archive's blocks, under a collection on every allocation.
cat > "$WORK/seam.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (turmeric stdlib/map))
(define (churn i)
  (if (= i 0) 'done
      (begin (list i i i i) (churn (- i 1)))))
(define m (map-assoc #map{:a 1} :k (list 1 2 3 "four" (vector 5 6))))
(churn 20000)
(define m2 (map-assoc m :s (string-append "hello" " world")))
(churn 20000)
(write (list (map-get m2 :k) (map-get m2 :s) (map-count m2)))
(newline)
EOF
seam_want='((1 2 3 "four" #(5 6)) "hello world" 3)'
if ! "$TUR" build "$WORK/seam.tur" -o "$WORK/seam" > "$WORK/seam.build" 2>&1; then
    echo "FAIL seam -- build failed: $(grep -m1 -i error "$WORK/seam.build" | cut -c1-160)"
else
    seam_got="$(TUR_GC_TORTURE=1 timeout 300 "$WORK/seam" 2> "$WORK/seam.err")"; rc=$?
    if [ "$rc" != 0 ]; then
        echo "FAIL seam -- exit $rc under TUR_GC_TORTURE=1: $(tail -1 "$WORK/seam.err" | cut -c1-120)"
    elif [ "$seam_got" != "$seam_want" ]; then
        echo "FAIL seam -- values kept in a Turmeric map came back as: $seam_got"
    else
        echo "PASS seam (values kept in a Turmeric map survive a collection on every allocation)"
    fi
fi | tee -a "$WORK/results"

# 3. A thread start is refused under the collector, and fine under TUR_R7RS_GC=0.
cat > "$WORK/spawner.tur" <<'EOF'
(defmodule spawner
  (export spawn-one)
  (defn worker [arg : ptr<void>] : ptr<void>
    ```c
    return arg;
    ```)
  (defn spawn-raw [f : ptr<void>] : ptr<void>
    ```c
    pthread_t *t = (pthread_t *)malloc(sizeof(pthread_t));
    if (pthread_create(t, NULL, (void *(*)(void *))f, NULL) != 0) { free(t); return NULL; }
    return (void *)t;
    ```)
  (defn join-raw [t : ptr<void>] : int
    ```c
    if (!t) return 0;
    pthread_join(*(pthread_t *)t, NULL);
    free(t);
    return 1;
    ```)
  (defn spawn-one [] : int
    (join-raw (spawn-raw worker))))
EOF
cat > "$WORK/threaded.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (turmeric spawner))
(display (spawn-one)) (newline)
EOF
thread_case() {
    local tag="$1"; shift
    if ! (cd "$WORK" && env "$@" "$TUR" build threaded.tur -o "threaded-$tag") > "$WORK/threaded-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    "$WORK/threaded-$tag" > "$WORK/threaded-$tag.out" 2> "$WORK/threaded-$tag.err"
    echo "$?"
}
plain_rc="$(thread_case plain TUR_R7RS_GC=0)"
gc_rc="$(thread_case gc TUR_R7RS_GC=1)"
if [ "$plain_rc" != 0 ] || [ "$(cat "$WORK/threaded-plain.out" 2>/dev/null)" != 1 ]; then
    echo "FAIL threads -- under TUR_R7RS_GC=0 the program should start and join a thread (exit $plain_rc)"
elif [ "$gc_rc" != 70 ]; then
    echo "FAIL threads -- under the collector a thread start should exit 70, got $gc_rc: $(tail -1 "$WORK/threaded-gc.err" | cut -c1-120)"
elif ! grep -q "r7rs-gc: this program starts a thread" "$WORK/threaded-gc.err"; then
    echo "FAIL threads -- the refusal did not say why: $(tail -1 "$WORK/threaded-gc.err" | cut -c1-120)"
elif ! grep -q "TUR_R7RS_GC=0" "$WORK/threaded-gc.err"; then
    echo "FAIL threads -- the refusal did not name the opt-out: $(tail -1 "$WORK/threaded-gc.err" | cut -c1-120)"
else
    echo "PASS threads (a thread start is refused under the collector, with the reason and the opt-out)"
fi | tee -a "$WORK/results"

# 4. Reclamation under an address-space limit.  `ulimit -v` binds nothing on
# macOS, so the check would read "fits both ways" there and say nothing.
if [ "$HOST" = "Linux" ]; then
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
    if ! env "$@" "$TUR" build "$WORK/churn.tur" -o "$WORK/churn-$tag" > "$WORK/churn-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    (ulimit -v 262144; "$WORK/churn-$tag" 2>/dev/null) 2>/dev/null || true
}
with="$(reclaim gc TUR_R7RS_GC=1)"
without="$(reclaim plain TUR_R7RS_GC=0)"
if [ "$with" != "done" ]; then
    echo "FAIL reclaim -- with the collector the churn did not fit in 256 MiB (got '$with')"
elif [ "$without" = "done" ]; then
    echo "FAIL reclaim -- the churn fits in 256 MiB WITHOUT the collector, so this check bites on nothing"
else
    echo "PASS reclaim (256 MiB: fits with the collector, not without)"
fi | tee -a "$WORK/results"
else
    echo "PASS reclaim (skipped on $HOST: no address-space limit to test under)" | tee -a "$WORK/results"
fi

pass=$(grep -c '^PASS' "$WORK/results")
fail=$(grep -c '^FAIL' "$WORK/results")
echo
echo "r7rs-gc: $pass passed, $fail failed (torture every $TORTURE allocations)"
[ "$fail" -eq 0 ]
