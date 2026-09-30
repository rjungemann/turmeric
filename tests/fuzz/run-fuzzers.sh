#!/usr/bin/env bash
# run-fuzzers.sh -- run every libFuzzer target in tests/fuzz for a fixed time.
#
#   bash tests/fuzz/run-fuzzers.sh <build-dir> [seconds-per-target] [findings-dir] [jobs]
#
# <build-dir> is a TUR_FUZZ build (tests/fuzz/README.md); the binaries are in
# <build-dir>/fuzz/.  Each target starts from its committed seeds
# (tests/fuzz/seeds/<target>/) plus inputs the tree already has -- every
# fixture .tur for the reader, every build.tur for the manifest reader, every
# .json for both JSON decoders, the repo's Justfile -- and runs for
# seconds-per-target (default 60) under ASan/UBSan, `jobs` targets at a time
# (default: nproc).
#
# Exit status: 0 when every target ran clean; 1 when any target stopped on a
# finding (its reproducer is under <findings-dir>/<target>/, and the target
# names are listed one per line in <findings-dir>/FAILED); 2 on a setup error.
# Reproduce a finding with:  <build-dir>/fuzz/<target> <findings-dir>/<target>/crash-...
set -u

BUILD=${1:?usage: run-fuzzers.sh <build-dir> [seconds] [findings-dir] [jobs]}
SECS=${2:-60}
OUT=${3:-fuzz-findings}
JOBS=${4:-$(nproc 2>/dev/null || echo 2)}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$BUILD/fuzz"

# TUR_FUZZ_TARGETS="fuzz_reader fuzz_manifest" narrows the run to those.
TARGETS=${TUR_FUZZ_TARGETS:-"fuzz_lsp_frame fuzz_image_header fuzz_serial_wire fuzz_reader
fuzz_manifest fuzz_json_interp fuzz_justfile fuzz_json_compiled
fuzz_serial_cont fuzz_httpd_head"}

for t in $TARGETS; do
    [ -x "$BIN/$t" ] || { echo "run-fuzzers: $BIN/$t missing -- build the TUR_FUZZ tree first" >&2; exit 2; }
done

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tur-fuzz-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
: > "$OUT/FAILED"

# Seed corpora.  A file in a corpus dir is one input; the reader and manifest
# targets read a selector byte first (see their harnesses).
for t in $TARGETS; do
    mkdir -p "$WORK/$t"
    cp "$ROOT/tests/fuzz/seeds/$t/"* "$WORK/$t/" 2>/dev/null || true
done
# (Each block runs only when its target is in this run's TARGETS.)
has() { [ -d "$WORK/$1" ]; }
i=0
if has fuzz_reader; then
    for f in "$ROOT"/tests/fixtures/*/*.tur "$ROOT"/examples/*/src/*.tur "$ROOT"/stdlib/*.tur; do
        [ -f "$f" ] || continue
        i=$((i + 1))
        { printf '0'; head -c 16384 "$f"; } > "$WORK/fuzz_reader/tree-$i.txt"
    done
    for f in "$ROOT"/tests/fixtures/*/*.tur.sweet "$ROOT"/tests/fixtures/*/*.sweet; do
        [ -f "$f" ] || continue
        i=$((i + 1))
        { printf '3'; head -c 16384 "$f"; } > "$WORK/fuzz_reader/tree-$i.txt"
    done
fi
if has fuzz_manifest; then
    find "$ROOT/tests" "$ROOT/examples" -name build.tur -o -name build.tur.sweet 2>/dev/null |
    while read -r f; do
        i=$((i + 1))
        case "$f" in *.sweet) sel=s ;; *) sel=p ;; esac
        { printf '%s' "$sel"; cat "$f"; } > "$WORK/fuzz_manifest/tree-$i.txt"
    done
fi
find "$ROOT/tests" "$ROOT/examples" -name '*.json' -size -64k 2>/dev/null |
while read -r f; do
    i=$((i + 1))
    for t in fuzz_json_interp fuzz_json_compiled; do
        has $t && cp "$f" "$WORK/$t/tree-$i.json"
    done
done
has fuzz_justfile && [ -f "$ROOT/Justfile" ] && cp "$ROOT/Justfile" "$WORK/fuzz_justfile/repo-Justfile"

run_one() {
    t=$1
    mkdir -p "$OUT/$t"
    # -close_fd_mask=3: the parsers under test print their own diagnostics
    # for every malformed input; libFuzzer keeps a private stderr for its
    # report and the sanitizers', so a finding is still printed in full.
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:allocator_may_return_null=1}" \
    UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
    "$BIN/$t" "$WORK/$t" \
        -max_total_time="$SECS" -timeout=10 -rss_limit_mb=2560 -max_len=65536 \
        -close_fd_mask=3 -print_final_stats=1 -artifact_prefix="$OUT/$t/" \
        > "$OUT/$t.log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL $t (exit $rc) -- see $OUT/$t.log"
        echo "$t" >> "$OUT/FAILED"
    else
        echo "ok   $t ($(grep -m1 -o 'stat::number_of_executed_units: [0-9]*' "$OUT/$t.log" | grep -o '[0-9]*$') runs)"
    fi
}
export -f run_one
export BIN WORK OUT SECS

echo "run-fuzzers: $SECS s per target, $JOBS at a time"
printf '%s\n' $TARGETS | xargs -P "$JOBS" -I{} bash -c 'run_one {}'

if [ -s "$OUT/FAILED" ]; then
    echo "run-fuzzers: findings in: $(tr '\n' ' ' < "$OUT/FAILED")"
    exit 1
fi
rm -f "$OUT/FAILED"
echo "run-fuzzers: all targets clean"
exit 0
