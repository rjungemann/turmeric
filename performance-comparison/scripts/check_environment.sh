#!/usr/bin/env bash
# Phase 4: environment validation
#
# Prints versions of all language runtimes, compiler flags, and hardware
# details used in benchmarking.  Cross-checks against the expected versions
# documented in docs/performance-comparison-plan.md.
#
# Usage:
#   ./scripts/check_environment.sh
#   ./scripts/check_environment.sh --json   # emit JSON to results/processed/environment.json

set -euo pipefail
cd "$(dirname "$0")/.."

EMIT_JSON=false
if [ "${1:-}" = "--json" ]; then
    EMIT_JSON=true
fi

# Expected versions (from performance-comparison-plan.md)
EXPECTED_CLANG="17"
EXPECTED_PYTHON="3.13"
EXPECTED_CLOJURE="1.12"
EXPECTED_RACKET="9"

pass=0
warn=0

# ── helper ───────────────────────────────────────────────────────────────────
check() {
    local label="$1" got="$2" expected_prefix="$3"
    if [[ "$got" == ${expected_prefix}* ]]; then
        printf "  [ok  ] %-20s %s\n" "$label" "$got"
        ((pass++)) || true
    else
        printf "  [WARN] %-20s got=%s  expected prefix=%s\n" "$label" "$got" "$expected_prefix"
        ((warn++)) || true
    fi
}

echo "=== Phase 4 Environment Validation ==="
echo

# ── hardware ─────────────────────────────────────────────────────────────────
echo "── Hardware ────────────────────────────────────────────"
CPU_MODEL=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)
MEM_BYTES=$(sysctl -n hw.memsize 2>/dev/null || echo "unknown")
MEM_GB=$(python3 -c "print(round($MEM_BYTES / (1024**3), 1))" 2>/dev/null || echo "?")
NCPU=$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo "?")
echo "  CPU:    $CPU_MODEL"
echo "  RAM:    ${MEM_GB} GB"
echo "  Cores:  $NCPU (logical)"
OS=$(sw_vers -productVersion 2>/dev/null || uname -r)
echo "  OS:     macOS $OS"
echo

# ── language runtimes ────────────────────────────────────────────────────────
echo "── Language Runtimes ──────────────────────────────────"

# C / clang
CLANG_VER=$(clang --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || echo "not found")
check "clang" "$CLANG_VER" "$EXPECTED_CLANG"

# Python
PYTHON_VER=$(python3 --version 2>/dev/null | awk '{print $2}' || echo "not found")
check "python3" "$PYTHON_VER" "$EXPECTED_PYTHON"

# Clojure
CLOJURE_VER=$(clojure --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || echo "not found")
check "clojure" "$CLOJURE_VER" "$EXPECTED_CLOJURE"

# Racket
RACKET_VER=$(racket --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "not found")
check "racket" "$RACKET_VER" "$EXPECTED_RACKET"

# Turmeric
TUR="$(pwd)/../../build-rel/tur"
if [ -x "$TUR" ]; then
    TUR_VER=$("$TUR" --version 2>/dev/null | head -1 || echo "(no --version)")
    printf "  [ok  ] %-20s %s\n" "turmeric (tur)" "$TUR_VER"
    ((pass++)) || true
else
    printf "  [WARN] %-20s binary not found at %s\n" "turmeric (tur)" "$TUR"
    ((warn++)) || true
fi

# Java (needed by Clojure)
JAVA_VER=$(java -version 2>&1 | head -1 | grep -oE '"[0-9]+[^"]*"' | tr -d '"' || echo "not found")
printf "  [info] %-20s %s\n" "java (for clojure)" "$JAVA_VER"

echo

# ── build flags ──────────────────────────────────────────────────────────────
echo "── Build Flags ────────────────────────────────────────"
echo "  C:        clang -O3 -lpthread -lm"
echo "  Turmeric: \$TUR build <src> -o <binary>"
echo "  Python:   python3 (no compilation)"
echo "  Clojure:  clojure -M <script>"
echo "  Racket:   racket <script>"
echo

# ── summary ──────────────────────────────────────────────────────────────────
echo "── Summary ────────────────────────────────────────────"
printf "  %d checks passed, %d warnings\n" "$pass" "$warn"
if [ "$warn" -gt 0 ]; then
    echo "  NOTE: Version mismatches may affect reproducibility of published results."
fi
echo

# ── optional JSON output ──────────────────────────────────────────────────────
if [ "$EMIT_JSON" = "true" ]; then
    mkdir -p results/processed
    python3 - <<PYEOF
import json, datetime

data = {
    "timestamp": datetime.datetime.utcnow().isoformat() + "Z",
    "hardware": {
        "cpu":   """$CPU_MODEL""",
        "ram_gb": $MEM_GB,
        "logical_cores": $NCPU,
        "os":    "macOS $OS",
    },
    "runtimes": {
        "clang":   "$CLANG_VER",
        "python3": "$PYTHON_VER",
        "clojure": "$CLOJURE_VER",
        "racket":  "$RACKET_VER",
        "java":    "$JAVA_VER",
    },
    "build_flags": {
        "c":       "clang -O3 -lpthread -lm",
        "python":  "interpreted",
        "clojure": "clojure -M",
        "racket":  "racket",
    },
    "checks_passed": $pass,
    "warnings":      $warn,
}
with open("results/processed/environment.json", "w") as f:
    json.dump(data, f, indent=2)
print("Environment data written to: results/processed/environment.json")
PYEOF
fi

# ── B0 (post-jit-benchmark-resurrection-plan): preflight matrix ──────────────
# What will actually run, per (benchmark, language), printed BEFORE a
# multi-hour sweep starts -- a missing column shows up here, not as a silent
# four-column result table.
echo
echo "── preflight matrix (source/binary present per language) ──"
BENCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/benchmarks"
RUST_DIR="$BENCH_DIR/rust-workspace/target/release"
HS_DIR="$BENCH_DIR/haskell-project/bin"
printf '%-18s %-17s %-3s %-4s %-5s %-4s %-4s %-8s %-8s %-7s %-7s\n' \
    CATEGORY BENCHMARK c tur turi jit rust haskell clojure racket python
for cat_dir in "$BENCH_DIR"/*/; do
    cat=$(basename "$cat_dir")
    [ "$cat" = "rust-workspace" ] && continue
    [ "$cat" = "haskell-project" ] && continue
    for tsrc in "$cat_dir"turmeric/*.tur; do
        [ -f "$tsrc" ] || continue
        b=$(basename "$tsrc" .tur)
        have() { [ -e "$1" ] && echo "y" || echo "-"; }
        printf '%-18s %-17s %-3s %-4s %-5s %-4s %-4s %-8s %-8s %-7s %-7s\n' \
            "$cat" "$b" \
            "$(have "$cat_dir/c/$b")" \
            "$(have "$cat_dir/turmeric/$b")" \
            "$(have "$cat_dir/turi/$b.tur")" \
            "$(have "$cat_dir/turmeric/$b.tur")" \
            "$(have "$RUST_DIR/$b")" \
            "$(have "$HS_DIR/$b")" \
            "$(have "$cat_dir/clojure/$b.clj")" \
            "$(have "$cat_dir/racket/$b.rkt")" \
            "$(have "$cat_dir/python/$b.py")"
    done
done
echo
echo "toolchains: $(command -v cc >/dev/null && echo cc || echo 'NO cc') |" \
     "$(command -v cargo >/dev/null && echo cargo || echo 'NO cargo') |" \
     "$(command -v ghc >/dev/null && echo ghc || echo 'NO ghc') |" \
     "$(command -v racket >/dev/null && echo racket || echo 'NO racket') |" \
     "$(command -v clojure >/dev/null && echo clojure || echo 'NO clojure') |" \
     "$(command -v python3 >/dev/null && echo python3 || echo 'NO python3')"
echo "note: 'tur'/'jit' columns list the prebuilt binary / jit-able source;"
echo "      binaries build during run_all.sh's build phase."
