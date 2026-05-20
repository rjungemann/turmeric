#!/usr/bin/env bash
# run_all.sh -- run all performance benchmarks across all languages.
#
# Usage:
#   ./scripts/run_all.sh [category] [size]
#
#   category  -- optional: numerical | data_structures | string_processing |
#                concurrency | memory | recursion  (default: all)
#   size      -- optional: small | medium | large  (default: small)
#
# Examples:
#   ./scripts/run_all.sh                   # all categories, small inputs
#   ./scripts/run_all.sh numerical         # numerical only, small inputs
#   ./scripts/run_all.sh numerical medium  # numerical, medium inputs
#
# Output: results/raw/<category>_<benchmark>_<language>_<size>_<timestamp>.json

set -euo pipefail
cd "$(dirname "$0")/.."

# ── configuration ────────────────────────────────────────────────────────────
BENCHMARK_DIR="$(pwd)/benchmarks"
INPUT_DIR="$(pwd)/inputs"
RESULTS_DIR="$(pwd)/results/raw"
TUR="$(pwd)/../../build-rel/tur"    # Turmeric release binary (relative to project root)
WARMUP_RUNS=3
MEASURE_RUNS=10

mkdir -p "$RESULTS_DIR"

# ── helpers ──────────────────────────────────────────────────────────────────

# elapsed_s <cmd> [args...] -- run command and return wall-clock seconds
elapsed_s() {
    local start end
    start=$(python3 -c "import time; print(time.perf_counter())")
    "$@" > /dev/null 2>&1
    end=$(python3 -c "import time; print(time.perf_counter())")
    python3 -c "print(round($end - $start, 6))"
}

# median of N numbers passed as lines to stdin
median_of() {
    python3 -c "
import sys, statistics
vals = [float(l) for l in sys.stdin if l.strip()]
if vals:
    print(round(statistics.median(vals), 6))
else:
    print('null')
"
}

# peak_rss_kb <cmd> [args...] -- peak RSS in KB (macOS /usr/bin/time -l)
peak_rss_kb() {
    /usr/bin/time -l "$@" 2>&1 | awk '/maximum resident/{print int($1/1024)}'
}

# run_timed <output_json> <language> <category> <benchmark> <size> <cmd> [args...]
# Runs WARMUP_RUNS warm-ups then MEASURE_RUNS timed iterations, emits JSON.
run_timed() {
    local out_json="$1" language="$2" category="$3" benchmark="$4" size="$5"
    shift 5
    local cmd=("$@")

    echo "  → $language/$benchmark [$size]..."

    # Correctness check — run once, capture stdout
    local output
    output=$("${cmd[@]}" 2>/dev/null) || {
        echo "    SKIP: command failed"
        return
    }

    # Warm-up
    local i
    for ((i=0; i<WARMUP_RUNS; i++)); do
        "${cmd[@]}" > /dev/null 2>&1 || true
    done

    # Timed runs
    local times=()
    for ((i=0; i<MEASURE_RUNS; i++)); do
        local t
        t=$(elapsed_s "${cmd[@]}")
        times+=("$t")
    done

    # Peak RSS (single run)
    local rss
    rss=$(peak_rss_kb "${cmd[@]}" 2>/dev/null) || rss="null"

    # Build JSON
    local times_json
    times_json=$(printf '%s\n' "${times[@]}" | python3 -c "
import sys, json, statistics
vals = [float(l) for l in sys.stdin if l.strip()]
print(json.dumps({
    'mean':   round(statistics.mean(vals), 6),
    'median': round(statistics.median(vals), 6),
    'stdev':  round(statistics.stdev(vals), 6) if len(vals) > 1 else 0,
    'min':    round(min(vals), 6),
    'max':    round(max(vals), 6),
    'runs':   vals,
}))")

    python3 -c "
import json, sys
data = {
    'language':  '$language',
    'category':  '$category',
    'benchmark': '$benchmark',
    'size':      '$size',
    'output':    $(python3 -c "import json; print(json.dumps('''$output'''.strip()))"),
    'timing_s':  $times_json,
    'peak_rss_kb': $([ "$rss" = "null" ] && echo "null" || echo "$rss"),
}
with open('$out_json', 'w') as f:
    json.dump(data, f, indent=2)
print(json.dumps(data))
" 2>/dev/null || echo "    WARNING: could not write $out_json"
}

# ── per-category benchmark runners ──────────────────────────────────────────

run_category() {
    local category="$1" size="$2"
    local bmark_dir="$BENCHMARK_DIR/$category"
    local input_dir="$INPUT_DIR/$category"

    echo "── $category [$size] ──────────────────────────────"

    case "$category" in
        numerical)
            _run_numerical "$bmark_dir" "$input_dir" "$size" ;;
        data_structures)
            _run_data_structures "$bmark_dir" "$input_dir" "$size" ;;
        string_processing)
            _run_string_processing "$bmark_dir" "$input_dir" "$size" ;;
        concurrency)
            _run_concurrency "$bmark_dir" "$input_dir" "$size" ;;
        memory)
            _run_memory "$bmark_dir" "$input_dir" "$size" ;;
        recursion)
            _run_recursion "$bmark_dir" "$input_dir" "$size" ;;
        *)
            echo "Unknown category: $category"; return 1 ;;
    esac
}

# ── numerical ────────────────────────────────────────────────────────────────
_run_numerical() {
    local dir="$1" idir="$2" size="$3"

    # fibonacci (iterative)
    local fib_n
    fib_n=$(python3 -c "import json; d=json.load(open('$idir/fibonacci.json')); print(d['$size']['n'])")
    _bench_lang numerical fibonacci "$size" "$fib_n" \
        c         "$dir/c/fibonacci" \
        turmeric  "$dir/turmeric/fibonacci" \
        clojure   "clojure -M $dir/clojure/fibonacci.clj" \
        racket    "racket $dir/racket/fibonacci.rkt" \
        python    "python3 $dir/python/fibonacci.py"

    # primes (sieve)
    local primes_n
    primes_n=$(python3 -c "import json; d=json.load(open('$idir/primes.json')); print(d['$size']['up_to'])")
    _bench_lang numerical primes "$size" "$primes_n" \
        c         "$dir/c/primes" \
        turmeric  "$dir/turmeric/primes" \
        clojure   "clojure -M $dir/clojure/primes.clj" \
        racket    "racket $dir/racket/primes.rkt" \
        python    "python3 $dir/python/primes.py"

    # matrix_multiply
    local mat_n
    mat_n=$(python3 -c "import json; d=json.load(open('$idir/matrix.json')); print(d['$size']['rows'])")
    _bench_lang numerical matrix_multiply "$size" "$mat_n" \
        c         "$dir/c/matrix_multiply" \
        turmeric  "$dir/turmeric/matrix_multiply" \
        clojure   "clojure -M $dir/clojure/matrix_multiply.clj" \
        racket    "racket $dir/racket/matrix_multiply.rkt" \
        python    "python3 $dir/python/matrix_multiply.py"

    # monte_carlo_pi
    local mc_n
    mc_n=$(python3 -c "import json; d=json.load(open('$idir/monte_carlo.json')); print(d['$size']['iterations'])")
    _bench_lang numerical monte_carlo_pi "$size" "$mc_n" \
        c         "$dir/c/monte_carlo_pi" \
        turmeric  "$dir/turmeric/monte_carlo_pi" \
        clojure   "clojure -M $dir/clojure/monte_carlo_pi.clj" \
        racket    "racket $dir/racket/monte_carlo_pi.rkt" \
        python    "python3 $dir/python/monte_carlo_pi.py"
}

# ── data structures ──────────────────────────────────────────────────────────
_run_data_structures() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/list_ops.json')); print(d['$size']['n'])")
    _bench_lang data_structures list_ops "$size" "$n" \
        c         "$dir/c/list_ops" \
        turmeric  "$dir/turmeric/list_ops" \
        clojure   "clojure -M $dir/clojure/list_ops.clj" \
        racket    "racket $dir/racket/list_ops.rkt" \
        python    "python3 $dir/python/list_ops.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/hash_map.json')); print(d['$size']['n'])")
    _bench_lang data_structures hash_map "$size" "$n" \
        c         "$dir/c/hash_map" \
        turmeric  "$dir/turmeric/hash_map" \
        clojure   "clojure -M $dir/clojure/hash_map.clj" \
        racket    "racket $dir/racket/hash_map.rkt" \
        python    "python3 $dir/python/hash_map.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/sort.json')); print(d['$size']['n'])")
    _bench_lang data_structures sort "$size" "$n" \
        c         "$dir/c/sort" \
        turmeric  "$dir/turmeric/sort" \
        clojure   "clojure -M $dir/clojure/sort.clj" \
        racket    "racket $dir/racket/sort.rkt" \
        python    "python3 $dir/python/sort.py"
}

# ── string processing ────────────────────────────────────────────────────────
_run_string_processing() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/string_concat.json')); print(d['$size']['n'])")
    _bench_lang string_processing string_concat "$size" "$n" \
        c         "$dir/c/string_concat" \
        turmeric  "$dir/turmeric/string_concat" \
        clojure   "clojure -M $dir/clojure/string_concat.clj" \
        racket    "racket $dir/racket/string_concat.rkt" \
        python    "python3 $dir/python/string_concat.py"

    local hs_n
    hs_n=$(python3 -c "import json; d=json.load(open('$idir/text_search.json')); print(d['$size']['haystack_size'])")
    _bench_lang string_processing text_search "$size" "$hs_n" \
        c         "$dir/c/text_search" \
        turmeric  "$dir/turmeric/text_search" \
        clojure   "clojure -M $dir/clojure/text_search.clj" \
        racket    "racket $dir/racket/text_search.rkt" \
        python    "python3 $dir/python/text_search.py"
}

# ── concurrency ──────────────────────────────────────────────────────────────
_run_concurrency() {
    local dir="$1" idir="$2" size="$3"

    local n_threads messages
    n_threads=$(python3 -c "import json; d=json.load(open('$idir/thread_ring.json')); print(d['$size']['n_threads'])")
    messages=$(python3 -c "import json; d=json.load(open('$idir/thread_ring.json')); print(d['$size']['messages'])")
    _bench_lang concurrency thread_ring "$size" "$n_threads $messages" \
        c         "$dir/c/thread_ring" \
        turmeric  "$dir/turmeric/thread_ring" \
        clojure   "clojure -M $dir/clojure/thread_ring.clj" \
        racket    "racket $dir/racket/thread_ring.rkt" \
        python    "python3 $dir/python/thread_ring.py"
}

# ── memory ───────────────────────────────────────────────────────────────────
_run_memory() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/alloc_churn.json')); print(d['$size']['n'])")
    _bench_lang memory alloc_churn "$size" "$n" \
        c         "$dir/c/alloc_churn" \
        turmeric  "$dir/turmeric/alloc_churn" \
        clojure   "clojure -M $dir/clojure/alloc_churn.clj" \
        racket    "racket $dir/racket/alloc_churn.rkt" \
        python    "python3 $dir/python/alloc_churn.py"
}

# ── recursion ────────────────────────────────────────────────────────────────
_run_recursion() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/fibonacci.json')); print(d['$size']['n'])")
    _bench_lang recursion fib_recursive "$size" "$n" \
        c         "$dir/c/fib_recursive" \
        turmeric  "$dir/turmeric/fib_recursive" \
        clojure   "clojure -M $dir/clojure/fib_recursive.clj" \
        racket    "racket $dir/racket/fib_recursive.rkt" \
        python    "python3 $dir/python/fib_recursive.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/factorial.json')); print(d['$size']['n'])")
    _bench_lang recursion factorial "$size" "$n" \
        c         "$dir/c/factorial" \
        turmeric  "$dir/turmeric/factorial" \
        clojure   "clojure -M $dir/clojure/factorial.clj" \
        racket    "racket $dir/racket/factorial.rkt" \
        python    "python3 $dir/python/factorial.py"
}

# ── _bench_lang: dispatch per language, skip missing impls ───────────────────
# Usage: _bench_lang <category> <benchmark> <size> <arg> \
#            <lang1> <cmd1_or_binary1> <lang2> <cmd2_or_binary2> ...
_bench_lang() {
    local category="$1" benchmark="$2" size="$3" arg="$4"
    shift 4

    while (( $# >= 2 )); do
        local lang="$1"
        local raw_cmd="$2"
        shift 2

        # Build the command array (handle space-separated commands)
        local cmd_arr
        read -ra cmd_arr <<< "$raw_cmd $arg"

        # Skip if primary binary/script doesn't exist
        local primary="${cmd_arr[0]}"
        # For clojure/racket/python, check the script file (last .clj/.rkt/.py arg)
        local impl_file
        case "$lang" in
            c|turmeric)
                impl_file="$primary" ;;
            *)
                # find the script file in the command
                for w in "${cmd_arr[@]}"; do
                    case "$w" in *.clj|*.rkt|*.py|*.tur) impl_file="$w" ;; esac
                done ;;
        esac

        if [ -z "${impl_file:-}" ] || [ ! -f "$impl_file" ] && [ ! -x "$impl_file" ]; then
            echo "  SKIP $lang/$benchmark (not implemented)"
            continue
        fi

        local ts
        ts=$(date +%Y%m%d_%H%M%S)
        local out_json="${RESULTS_DIR}/${category}_${benchmark}_${lang}_${size}_${ts}.json"

        run_timed "$out_json" "$lang" "$category" "$benchmark" "$size" "${cmd_arr[@]}" || true
    done
}

# ── build C and Turmeric binaries before benchmarking ────────────────────────
build_c_binaries() {
    echo "── building C binaries ──────────────────────────────"
    local categories=(numerical data_structures string_processing concurrency memory recursion)
    for cat in "${categories[@]}"; do
        local cdir="$BENCHMARK_DIR/$cat/c"
        [ -d "$cdir" ] || continue
        for src in "$cdir"/*.c; do
            [ -f "$src" ] || continue
            local bin="${src%.c}"
            clang -O3 -o "$bin" "$src" -lpthread 2>/dev/null && echo "  built $bin" || echo "  WARN: failed to build $bin"
        done
    done
}

build_turmeric_binaries() {
    echo "── building Turmeric binaries ───────────────────────"
    local categories=(numerical data_structures string_processing concurrency memory recursion)
    for cat in "${categories[@]}"; do
        local tdir="$BENCHMARK_DIR/$cat/turmeric"
        [ -d "$tdir" ] || continue
        for src in "$tdir"/*.tur; do
            [ -f "$src" ] || continue
            local bin="${src%.tur}"
            "$TUR" build "$src" -o "$bin" 2>/dev/null && echo "  built $bin" || echo "  WARN: failed to build $bin"
        done
    done
}

# ── main ─────────────────────────────────────────────────────────────────────
ALL_CATEGORIES=(numerical data_structures string_processing concurrency memory recursion)

TARGET_CATEGORY="${1:-all}"
TARGET_SIZE="${2:-small}"

build_c_binaries
build_turmeric_binaries

if [ "$TARGET_CATEGORY" = "all" ]; then
    for cat in "${ALL_CATEGORIES[@]}"; do
        run_category "$cat" "$TARGET_SIZE"
    done
else
    run_category "$TARGET_CATEGORY" "$TARGET_SIZE"
fi

echo
echo "── done ─────────────────────────────────────────────────"
echo "Results written to: $RESULTS_DIR"
