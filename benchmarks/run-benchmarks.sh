#!/usr/bin/env bash
# benchmarks/run-benchmarks.sh — benchmark runner for Turmeric HKT (Phase §8)
#
# Measures the overhead of dictionary passing vs. monomorphic direct calls.
#
# Layout:
#   benchmarks/<name>.tur              — benchmark source
#   benchmarks/<name>-baseline.c      — monomorphic C baseline (optional)
#   benchmarks/expected.time          — upper bound in milliseconds (optional)
#
# Usage:
#   ./benchmarks/run-benchmarks.sh    — run all benchmarks
#   ./benchmarks/run-benchmarks.sh <name> — run specific benchmark

set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "benchmarks: $TUR not built; run 'cmake --build build --target tur' first" >&2; exit 2; }

# Default compiler flags for benchmark builds
# Use -O2 for realistic performance measurement
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall}"

# Output directory for benchmark artifacts
OUTPUT_DIR="./benchmarks/output"
RESULTS_FILE="./benchmarks/benchmark-results.md"

mkdir -p "$OUTPUT_DIR"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Benchmark timeout in seconds (default: 60)
TIMEOUT=${BENCHMARK_TIMEOUT:-60}

# Function to measure execution time of a command
measure_time() {
    local cmd="$1"
    local start end
    start=$(date +%s%N)
    eval "$cmd" >/dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))  # Return milliseconds
}

# Function to compile and run a benchmark
run_benchmark() {
    local name="$1"
    local tur_file="$2"
    local c_file="$OUTPUT_DIR/${name}.c"
    local exe_file="$OUTPUT_DIR/${name}"
    local baseline_c="${tur_file%.tur}-baseline.c"
    local baseline_exe="$OUTPUT_DIR/${name}-baseline"
    local expected_time="${tur_file%.tur}.time"
    
    echo -n "Running benchmark: ${name}... "
    
    # Compile the benchmark to C
    if ! $TUR build "$tur_file" -o "$c_file" >/dev/null 2>&1; then
        echo -e "${RED}FAIL${NC} (compilation failed)"
        return 1
    fi
    
    # Compile the C to executable
    if ! $BUILD_CC $TUR_CC_FLAGS -I. -o "$exe_file" "$c_file" >/dev/null 2>&1; then
        echo -e "${RED}FAIL${NC} (C compilation failed)"
        return 1
    fi
    
    # Measure execution time
    local elapsed
    elapsed=$(measure_time "$exe_file")
    
    # Check against expected time if present
    local passed=true
    if [ -f "$expected_time" ]; then
        local expected
        expected=$(cat "$expected_time")
        if [ "$elapsed" -gt "$expected" ]; then
            passed=false
        fi
    fi
    
    if $passed; then
        echo -e "${GREEN}PASS${NC} (${elapsed}ms)"
        echo "- **${name}**: ${elapsed}ms" >> "$RESULTS_FILE"
    else
        echo -e "${RED}FAIL${NC} (${elapsed}ms > ${expected}ms expected)"
        echo "- **${name}**: ${elapsed}ms (EXCEEDED ${expected}ms)" >> "$RESULTS_FILE"
    fi
    
    # If baseline exists, compute overhead ratio
    if [ -f "$baseline_c" ]; then
        # Compile baseline
        if $BUILD_CC $TUR_CC_FLAGS -I. -o "$baseline_exe" "$baseline_c" >/dev/null 2>&1; then
            local baseline_elapsed
            baseline_elapsed=$(measure_time "$baseline_exe")
            
            if [ "$baseline_elapsed" -gt 0 ]; then
                local ratio
                ratio=$(echo "scale=2; $elapsed / $baseline_elapsed" | bc)
                echo "  Baseline: ${baseline_elapsed}ms, Overhead ratio: ${ratio}x"
                echo "  - Baseline: ${baseline_elapsed}ms, Overhead ratio: ${ratio}x" >> "$RESULTS_FILE"
                
                # Check if overhead exceeds 2x
                local ratio_int
                ratio_int=$(echo "$ratio * 100" | bc | cut -d. -f1)
                if [ "$ratio_int" -gt 200 ]; then
                    echo -e "  ${YELLOW}WARNING${NC}: Overhead exceeds 2x - consider filing an issue"
                    echo "  - **WARNING**: Overhead exceeds 2x" >> "$RESULTS_FILE"
                fi
            fi
        fi
    fi
    
    return 0
}

# Main
echo "========================================"
echo "Turmeric HKT Dictionary Passing Benchmarks"
echo "========================================"
echo ""

# Initialize results file
> "$RESULTS_FILE"
echo "# Dictionary Passing Benchmark Results" >> "$RESULTS_FILE"
echo "Generated: $(date)" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"
echo "## Results" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"

PASS=0
FAIL=0

# Find all .tur files in benchmarks directory
if [ $# -eq 0 ]; then
    # Run all benchmarks
    for tur_file in benchmarks/*.tur; do
        if [ -f "$tur_file" ]; then
            name=$(basename "$tur_file" .tur)
            if run_benchmark "$name" "$tur_file"; then
                PASS=$((PASS + 1))
            else
                FAIL=$((FAIL + 1))
            fi
            echo ""
        fi
    done
else
    # Run specific benchmark
    for name in "$@"; do
        tur_file="benchmarks/${name}.tur"
        if [ -f "$tur_file" ]; then
            if run_benchmark "$name" "$tur_file"; then
                PASS=$((PASS + 1))
            else
                FAIL=$((FAIL + 1))
            fi
        else
            echo "Benchmark '$name' not found"
            FAIL=$((FAIL + 1))
        fi
    done
fi

echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
echo "========================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi

exit 0
