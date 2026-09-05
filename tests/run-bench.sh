#!/usr/bin/env bash
# tests/run-bench.sh — Benchmark runner for Turmeric (Phase HKT-P7)
#
# Measures the performance of benchmark fixtures and reports wall-time statistics.
#
# Benchmark fixture convention:
#   - tests/benchmarks/<name>.tur : benchmark source with (defn bench-main [] ...)
#   - tests/benchmarks/<name>.min-iterations : minimum iterations (default: 1000)
#
# Usage:
#   ./tests/run-bench.sh                    — run all benchmarks
#   ./tests/run-bench.sh <name>             — run specific benchmark
#   BENCHMINIT=<n> ./tests/run-bench.sh     — override default min iterations

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "Error: $TUR not built; run 'cmake --build build' first" >&2; exit 2; }

# Default minimum iterations
DEFAULT_MIN_ITERATIONS=1000

# Use clock_gettime(CLOCK_MONOTONIC) for high-resolution timing
# Falls back to date +%s%N on systems without clock_gettime
use_clock_gettime=true
if ! command -v clock_gettime >/dev/null 2>&1; then
    # clock_gettime might be in libc, try linking test
    use_clock_gettime=false
fi

# Output directory
OUTPUT_DIR="./tests/benchmarks/output"
mkdir -p "$OUTPUT_DIR"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Function to get nanoseconds using clock_gettime or fallback
get_nanos() {
    if $use_clock_gettime; then
        # Create a tiny C program to call clock_gettime
        local tmp_c="$OUTPUT_DIR/tmp_time.c"
        local tmp_exe="$OUTPUT_DIR/tmp_time"
        cat > "$tmp_c" <<'CEOF'
#include <stdio.h>
#include <time.h>
int main() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%lld%09ld", (long long)ts.tv_sec, ts.tv_nsec);
    return 0;
}
CEOF
        if gcc -o "$tmp_exe" "$tmp_c" -rtlib 2>/dev/null || gcc -o "$tmp_exe" "$tmp_c" 2>/dev/null; then
            "$tmp_exe"
            rm -f "$tmp_c" "$tmp_exe"
            return
        fi
        use_clock_gettime=false
    fi
    # Fallback to date +%s%N (macOS)
    date +%s%N
}

# Function to measure execution time of a Turmeric benchmark
# Returns time in milliseconds
measure_benchmark() {
    local tur_file="$1"
    local c_file="$OUTPUT_DIR/$(basename "$tur_file" .tur).c"
    local exe_file="$OUTPUT_DIR/$(basename "$tur_file" .tur)"
    local min_iterations="$2"
    local name=$(basename "$tur_file" .tur)
    
    # Compile the benchmark to C
    if ! $TUR emit-c "$tur_file" > "$c_file" 2>/dev/null; then
        echo -e "${RED}FAIL${NC} (emit-c failed)"
        return 1
    fi
    
    # Compile the C to executable with optimizations
    local cc_flags="-O2 -std=c99 -Wall"
    if ! cc $cc_flags -I. -o "$exe_file" "$c_file" >/dev/null 2>&1; then
        echo -e "${RED}FAIL${NC} (C compilation failed)"
        return 1
    fi
    
    # Measure execution time: run min_iterations times
    local start_ns end_ns
    start_ns=$(get_nanos)
    
    for ((i = 0; i < min_iterations; i++)); do
        "$exe_file" >/dev/null 2>&1
    done
    
    end_ns=$(get_nanos)
    
    # Calculate total milliseconds
    local total_ms=$(( (end_ns - start_ns) / 1000000 ))
    local per_iter_ms=$(( total_ms / min_iterations ))
    
    echo "$per_iter_ms"
    return 0
}

# Function to run a single benchmark
run_benchmark() {
    local name="$1"
    local tur_file="tests/benchmarks/${name}.tur"
    local min_iterations_file="tests/benchmarks/${name}.min-iterations"
    local min_iterations=${BENCHMINIT:-$DEFAULT_MIN_ITERATIONS}
    
    # Check for fixture-specific min-iterations
    if [ -f "$min_iterations_file" ]; then
        min_iterations=$(cat "$min_iterations_file")
    fi
    
    echo -n "Running benchmark: ${name} (${min_iterations} iterations)... "
    
    local elapsed
    elapsed=$(measure_benchmark "$tur_file" "$min_iterations")
    local rc=$?
    
    if [ $rc -ne 0 ]; then
        echo -e "${RED}FAIL${NC}"
        return 1
    fi
    
    echo -e "${GREEN}PASS${NC} (${elapsed}ms per iteration)"
    return 0
}

# Main
PASS=0
FAIL=0

echo "========================================"
echo "Turmeric Benchmark Suite (HKT-P7)"
echo "========================================"
echo ""

if [ $# -eq 0 ]; then
    # Run all benchmarks in tests/benchmarks/
    if [ -d "tests/benchmarks" ]; then
        for tur_file in tests/benchmarks/*.tur; do
            if [ -f "$tur_file" ]; then
                name=$(basename "$tur_file" .tur)
                if run_benchmark "$name"; then
                    PASS=$((PASS + 1))
                else
                    FAIL=$((FAIL + 1))
                fi
                echo ""
            fi
        done
    else
        echo "No tests/benchmarks directory found"
        FAIL=1
    fi
else
    # Run specific benchmarks
    for name in "$@"; do
        if run_benchmark "$name"; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
        fi
        echo ""
    done
fi

echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
echo "========================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi

exit 0
