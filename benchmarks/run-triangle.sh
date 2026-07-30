#!/usr/bin/env bash
# benchmarks/run-triangle.sh -- the J3 engine triangle (jit-engine-plan
# section 5): interpreter vs `tur jit` vs `tur build` + cc -O2, over the
# pure-Turmeric programs in benchmarks/triangle/.
#
# Measures END-TO-END wall time per engine (what a user waits for from a
# cold start: compile + run together), plus the cc path split into its
# build and run phases so steady-state native runtime is visible next to
# the compile latency the JIT exists to cut.  Best of N (default 3).
#
# Usage:
#   TUR=./build-turjit/tur bash benchmarks/run-triangle.sh
#
# Use a RELEASE TUR_JIT=ON build: a Debug/ASan binary skews every leg.
# Output is a markdown table (docs/guides/performance-guide.md carries a
# representative snapshot; rerun this to refresh it).

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build-turjit/tur}"
[ -x "$TUR" ] || { echo "run-triangle: no tur at $TUR (need a TUR_JIT=ON build)" >&2; exit 2; }
probe=$("$TUR" --enable=jit jit 2>&1 || true)
case "$probe" in
  *"usage: tur jit"*) ;;
  *) echo "run-triangle: $TUR carries no JIT engine (configure -DTUR_JIT=ON)"; exit 2 ;;
esac

REPS="${REPS:-3}"
OUT="${OUT:-benchmarks/output/triangle}"
mkdir -p "$OUT"
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -fno-strict-aliasing -L${_tur_build_dir}/src}"

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# best-of-REPS wall time of "$@" in ms; output discarded.
best() {
    local best_ms=""
    for _ in $(seq "$REPS"); do
        local s e
        s=$(now_ms)
        "$@" >/dev/null 2>&1
        e=$(now_ms)
        local d=$((e - s))
        if [ -z "$best_ms" ] || [ "$d" -lt "$best_ms" ]; then best_ms=$d; fi
    done
    echo "$best_ms"
}

echo "| program | interpreter | tur jit | cc build | cc run | cc total |"
echo "|---|---|---|---|---|---|"
for f in benchmarks/triangle/*.tur; do
    name=$(basename "$f" .tur)
    exe="$OUT/$name"

    # Outputs must agree across engines before any number means anything.
    o_interp=$("$TUR" --interpret "$f" 2>/dev/null | tail -1)
    o_jit=$("$TUR" --enable=jit jit "$f" 2>/dev/null | tail -1)
    "$TUR" build "$f" -o "$exe" >/dev/null 2>&1
    o_cc=$("$exe" 2>/dev/null | tail -1)
    if [ "$o_interp" != "$o_cc" ] || [ "$o_jit" != "$o_cc" ]; then
        echo "| $name | DIVERGED: interp='$o_interp' jit='$o_jit' cc='$o_cc' | | | | |"
        continue
    fi

    t_interp=$(best "$TUR" --interpret "$f")
    t_jit=$(best "$TUR" --enable=jit jit "$f")
    t_build=$(best "$TUR" build "$f" -o "$exe")
    t_run=$(best "$exe")
    echo "| $name | ${t_interp}ms | ${t_jit}ms | ${t_build}ms | ${t_run}ms | $((t_build + t_run))ms |"
done
