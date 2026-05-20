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
TUR="${TUR:-$(pwd)/../build-rel/tur}"  # Turmeric build binary (override with TUR env var)
RUST_RELEASE_DIR="$(pwd)/benchmarks/rust-workspace/target/release"  # Rust release binaries
WARMUP_RUNS=3
MEASURE_RUNS=10
BENCHMARK_TIMEOUT_S=60   # per-invocation timeout (seconds)

mkdir -p "$RESULTS_DIR"

# ── helpers ──────────────────────────────────────────────────────────────────

# Detect a working timeout command (macOS needs coreutils gtimeout)
if command -v gtimeout &>/dev/null; then
    _TIMEOUT_CMD=gtimeout
elif command -v timeout &>/dev/null; then
    _TIMEOUT_CMD=timeout
else
    _TIMEOUT_CMD=""
fi

# with_timeout <cmd> [args...] -- run with BENCHMARK_TIMEOUT_S timeout if available
with_timeout() {
    if [ -n "${_TIMEOUT_CMD:-}" ]; then
        "$_TIMEOUT_CMD" "$BENCHMARK_TIMEOUT_S" "$@"
    else
        "$@"
    fi
}

# elapsed_s <cmd> [args...] -- run command and return wall-clock seconds
elapsed_s() {
    local start end
    start=$(python3 -c "import time; print(time.perf_counter())")
    with_timeout "$@" > /dev/null 2>&1
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
    output=$(with_timeout "${cmd[@]}" 2>/dev/null) || {
        echo "    SKIP: command failed or timed out"
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
    rss=$(peak_rss_kb "${cmd[@]}" 2>/dev/null) || rss="null"   # peak_rss_kb uses /usr/bin/time; timeout applies inside elapsed_s

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
        io)
            _run_io "$bmark_dir" "$input_dir" "$size" ;;
        real_world)
            _run_real_world "$bmark_dir" "$input_dir" "$size" ;;
        micro)
            _run_micro "$bmark_dir" "$input_dir" "$size" ;;
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
        turi      "$TUR --interpret$dir/turi/fibonacci.tur" \
        rust      "$RUST_RELEASE_DIR/fibonacci" \
        python    "python3 $dir/python/fibonacci.py"

    # primes (sieve)
    local primes_n
    primes_n=$(python3 -c "import json; d=json.load(open('$idir/primes.json')); print(d['$size']['up_to'])")
    _bench_lang numerical primes "$size" "$primes_n" \
        c         "$dir/c/primes" \
        turmeric  "$dir/turmeric/primes" \
        clojure   "clojure -M $dir/clojure/primes.clj" \
        racket    "racket $dir/racket/primes.rkt" \
        turi      "$TUR --interpret$dir/turi/primes.tur" \
        rust      "$RUST_RELEASE_DIR/primes" \
        python    "python3 $dir/python/primes.py"

    # matrix_multiply
    local mat_n
    mat_n=$(python3 -c "import json; d=json.load(open('$idir/matrix.json')); print(d['$size']['rows'])")
    _bench_lang numerical matrix_multiply "$size" "$mat_n" \
        c         "$dir/c/matrix_multiply" \
        turmeric  "$dir/turmeric/matrix_multiply" \
        clojure   "clojure -M $dir/clojure/matrix_multiply.clj" \
        racket    "racket $dir/racket/matrix_multiply.rkt" \
        turi      "$TUR --interpret$dir/turi/matrix_multiply.tur" \
        rust      "$RUST_RELEASE_DIR/matrix_multiply" \
        python    "python3 $dir/python/matrix_multiply.py"

    # monte_carlo_pi
    local mc_n
    mc_n=$(python3 -c "import json; d=json.load(open('$idir/monte_carlo.json')); print(d['$size']['iterations'])")
    _bench_lang numerical monte_carlo_pi "$size" "$mc_n" \
        c         "$dir/c/monte_carlo_pi" \
        turmeric  "$dir/turmeric/monte_carlo_pi" \
        clojure   "clojure -M $dir/clojure/monte_carlo_pi.clj" \
        racket    "racket $dir/racket/monte_carlo_pi.rkt" \
        turi      "$TUR --interpret$dir/turi/monte_carlo_pi.tur" \
        rust      "$RUST_RELEASE_DIR/monte_carlo_pi" \
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
        turi      "$TUR --interpret$dir/turi/list_ops.tur" \
        rust      "$RUST_RELEASE_DIR/list_ops" \
        python    "python3 $dir/python/list_ops.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/hash_map.json')); print(d['$size']['n'])")
    _bench_lang data_structures hash_map "$size" "$n" \
        c         "$dir/c/hash_map" \
        turmeric  "$dir/turmeric/hash_map" \
        clojure   "clojure -M $dir/clojure/hash_map.clj" \
        racket    "racket $dir/racket/hash_map.rkt" \
        turi      "$TUR --interpret$dir/turi/hash_map.tur" \
        rust      "$RUST_RELEASE_DIR/hash_map" \
        python    "python3 $dir/python/hash_map.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/sort.json')); print(d['$size']['n'])")
    _bench_lang data_structures sort "$size" "$n" \
        c         "$dir/c/sort" \
        turmeric  "$dir/turmeric/sort" \
        clojure   "clojure -M $dir/clojure/sort.clj" \
        racket    "racket $dir/racket/sort.rkt" \
        turi      "$TUR --interpret$dir/turi/sort.tur" \
        rust      "$RUST_RELEASE_DIR/sort" \
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
        turi      "$TUR --interpret$dir/turi/string_concat.tur" \
        rust      "$RUST_RELEASE_DIR/string_concat" \
        python    "python3 $dir/python/string_concat.py"

    local hs_n
    hs_n=$(python3 -c "import json; d=json.load(open('$idir/text_search.json')); print(d['$size']['haystack_size'])")
    _bench_lang string_processing text_search "$size" "$hs_n" \
        c         "$dir/c/text_search" \
        turmeric  "$dir/turmeric/text_search" \
        clojure   "clojure -M $dir/clojure/text_search.clj" \
        racket    "racket $dir/racket/text_search.rkt" \
        turi      "$TUR --interpret$dir/turi/text_search.tur" \
        rust      "$RUST_RELEASE_DIR/text_search" \
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
        turi      "$TUR --interpret$dir/turi/thread_ring.tur" \
        rust      "$RUST_RELEASE_DIR/thread_ring" \
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
        turi      "$TUR --interpret$dir/turi/alloc_churn.tur" \
        rust      "$RUST_RELEASE_DIR/alloc_churn" \
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
        turi      "$TUR --interpret$dir/turi/fib_recursive.tur" \
        rust      "$RUST_RELEASE_DIR/fib_recursive" \
        python    "python3 $dir/python/fib_recursive.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/factorial.json')); print(d['$size']['n'])")
    _bench_lang recursion factorial "$size" "$n" \
        c         "$dir/c/factorial" \
        turmeric  "$dir/turmeric/factorial" \
        clojure   "clojure -M $dir/clojure/factorial.clj" \
        racket    "racket $dir/racket/factorial.rkt" \
        turi      "$TUR --interpret$dir/turi/factorial.tur" \
        rust      "$RUST_RELEASE_DIR/factorial" \
        python    "python3 $dir/python/factorial.py"
}

# ── io ───────────────────────────────────────────────────────────────────────
_run_io() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/file_write.json')); print(d['$size']['n'])")
    _bench_lang io file_write "$size" "$n" \
        c         "$dir/c/file_write" \
        turmeric  "$dir/turmeric/file_write" \
        clojure   "clojure -M $dir/clojure/file_write.clj" \
        racket    "racket $dir/racket/file_write.rkt" \
        turi      "$TUR --interpret$dir/turi/file_write.tur" \
        rust      "$RUST_RELEASE_DIR/file_write" \
        python    "python3 $dir/python/file_write.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/file_read.json')); print(d['$size']['n'])")
    _bench_lang io file_read "$size" "$n" \
        c         "$dir/c/file_read" \
        turmeric  "$dir/turmeric/file_read" \
        clojure   "clojure -M $dir/clojure/file_read.clj" \
        racket    "racket $dir/racket/file_read.rkt" \
        turi      "$TUR --interpret$dir/turi/file_read.tur" \
        rust      "$RUST_RELEASE_DIR/file_read" \
        python    "python3 $dir/python/file_read.py"

    local fsize nreads
    fsize=$(python3 -c "import json; d=json.load(open('$idir/random_access.json')); print(d['$size']['file_size'])")
    nreads=$(python3 -c "import json; d=json.load(open('$idir/random_access.json')); print(d['$size']['n_reads'])")
    _bench_lang io random_access "$size" "$fsize $nreads" \
        c         "$dir/c/random_access" \
        turmeric  "$dir/turmeric/random_access" \
        clojure   "clojure -M $dir/clojure/random_access.clj" \
        racket    "racket $dir/racket/random_access.rkt" \
        turi      "$TUR --interpret$dir/turi/random_access.tur" \
        rust      "$RUST_RELEASE_DIR/random_access" \
        python    "python3 $dir/python/random_access.py"
}

# ── real_world ───────────────────────────────────────────────────────────────
_run_real_world() {
    local dir="$1" idir="$2" size="$3"

    local nb_bodies nb_steps
    nb_bodies=$(python3 -c "import json; d=json.load(open('$idir/nbody.json')); print(d['$size']['n_bodies'])")
    nb_steps=$(python3 -c "import json; d=json.load(open('$idir/nbody.json')); print(d['$size']['steps'])")
    _bench_lang real_world nbody "$size" "$nb_bodies $nb_steps" \
        c         "$dir/c/nbody" \
        turmeric  "$dir/turmeric/nbody" \
        clojure   "clojure -M $dir/clojure/nbody.clj" \
        racket    "racket $dir/racket/nbody.rkt" \
        turi      "$TUR --interpret$dir/turi/nbody.tur" \
        rust      "$RUST_RELEASE_DIR/nbody" \
        python    "python3 $dir/python/nbody.py"

    local rt_w rt_h
    rt_w=$(python3 -c "import json; d=json.load(open('$idir/ray_tracing.json')); print(d['$size']['width'])")
    rt_h=$(python3 -c "import json; d=json.load(open('$idir/ray_tracing.json')); print(d['$size']['height'])")
    _bench_lang real_world ray_tracing "$size" "$rt_w $rt_h" \
        c         "$dir/c/ray_tracing" \
        turmeric  "$dir/turmeric/ray_tracing" \
        clojure   "clojure -M $dir/clojure/ray_tracing.clj" \
        racket    "racket $dir/racket/ray_tracing.rkt" \
        turi      "$TUR --interpret$dir/turi/ray_tracing.tur" \
        rust      "$RUST_RELEASE_DIR/ray_tracing" \
        python    "python3 $dir/python/ray_tracing.py"
}

# ── micro ────────────────────────────────────────────────────────────────────
_run_micro() {
    local dir="$1" idir="$2" size="$3"

    local n
    n=$(python3 -c "import json; d=json.load(open('$idir/int_arith.json')); print(d['$size']['n'])")
    _bench_lang micro int_arith "$size" "$n" \
        c         "$dir/c/int_arith" \
        turmeric  "$dir/turmeric/int_arith" \
        clojure   "clojure -M $dir/clojure/int_arith.clj" \
        racket    "racket $dir/racket/int_arith.rkt" \
        turi      "$TUR --interpret$dir/turi/int_arith.tur" \
        rust      "$RUST_RELEASE_DIR/int_arith" \
        python    "python3 $dir/python/int_arith.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/float_arith.json')); print(d['$size']['n'])")
    _bench_lang micro float_arith "$size" "$n" \
        c         "$dir/c/float_arith" \
        turmeric  "$dir/turmeric/float_arith" \
        clojure   "clojure -M $dir/clojure/float_arith.clj" \
        racket    "racket $dir/racket/float_arith.rkt" \
        turi      "$TUR --interpret$dir/turi/float_arith.tur" \
        rust      "$RUST_RELEASE_DIR/float_arith" \
        python    "python3 $dir/python/float_arith.py"

    n=$(python3 -c "import json; d=json.load(open('$idir/function_call.json')); print(d['$size']['n'])")
    _bench_lang micro function_call "$size" "$n" \
        c         "$dir/c/function_call" \
        turmeric  "$dir/turmeric/function_call" \
        clojure   "clojure -M $dir/clojure/function_call.clj" \
        racket    "racket $dir/racket/function_call.rkt" \
        turi      "$TUR --interpret$dir/turi/function_call.tur" \
        rust      "$RUST_RELEASE_DIR/function_call" \
        python    "python3 $dir/python/function_call.py"
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
            c|turmeric|rust)
                impl_file="$primary" ;;
            turi)
                # turi runs via $TUR --interpret <file.tur>; find the .tur arg
                for w in "${cmd_arr[@]}"; do
                    case "$w" in *.tur) impl_file="$w" ;; esac
                done ;;
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

        case "$lang" in
            turi)
                # tur run needs to be launched from the project root so that
                # stdlib/ is found at its default relative path.
                ( cd "$(dirname "$TUR")/.." && run_timed "$out_json" "$lang" "$category" "$benchmark" "$size" "${cmd_arr[@]}" ) || true ;;
            *)
                run_timed "$out_json" "$lang" "$category" "$benchmark" "$size" "${cmd_arr[@]}" || true ;;
        esac
    done
}

# ── build C and Turmeric binaries before benchmarking ────────────────────────
build_c_binaries() {
    echo "── building C binaries ──────────────────────────────"
    local categories=(numerical data_structures string_processing concurrency memory recursion io real_world micro)
    for cat in "${categories[@]}"; do
        local cdir="$BENCHMARK_DIR/$cat/c"
        [ -d "$cdir" ] || continue
        for src in "$cdir"/*.c; do
            [ -f "$src" ] || continue
            local bin="${src%.c}"
            clang -O3 -o "$bin" "$src" -lpthread -lm 2>/dev/null && echo "  built $bin" || echo "  WARN: failed to build $bin"
        done
    done
}

build_turmeric_binaries() {
    echo "── building Turmeric binaries ───────────────────────"
    local categories=(numerical data_structures string_processing concurrency memory recursion io real_world micro)
    for cat in "${categories[@]}"; do
        local tdir="$BENCHMARK_DIR/$cat/turmeric"
        [ -d "$tdir" ] || continue
        for src in "$tdir"/*.tur; do
            [ -f "$src" ] || continue
            local bin="${src%.tur}"
            ( cd "$(dirname "$TUR")/.." && "$TUR" build "$src" -o "$bin" ) 2>/dev/null && echo "  built $bin" || echo "  WARN: failed to build $bin"
        done
    done
}

build_rust_binaries() {
    echo "── building Rust binaries ───────────────────────────"
    local rwdir="$BENCHMARK_DIR/rust-workspace"
    if [ -d "$rwdir" ]; then
        (cd "$rwdir" && cargo build --workspace --release 2>&1 | tail -5) \
            && echo "  Rust workspace built" \
            || echo "  WARN: Rust build failed"
    else
        echo "  SKIP: rust-workspace not found"
    fi
}

# ── main ─────────────────────────────────────────────────────────────────────
ALL_CATEGORIES=(numerical data_structures string_processing concurrency memory recursion io real_world micro)

TARGET_CATEGORY="${1:-all}"
TARGET_SIZE="${2:-small}"

build_c_binaries
build_turmeric_binaries
build_rust_binaries

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
