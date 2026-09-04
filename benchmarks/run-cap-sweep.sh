#!/usr/bin/env bash
# benchmarks/run-cap-sweep.sh -- SX0(b): which refinement-solver cap bites, and
# how often.
#
# Every cap in the staged decision procedure (S0..S3) degrades to RT_UNKNOWN
# when it bites, which is sound but silent -- from the outside, an obligation
# the solver capped out on and one that was never in its competence look
# identical.  This sweep makes the difference countable across the three
# populations the solver-extension plan names, and writes a markdown table to
# benchmarks/cap-sweep-results.md.
#
# The output is decision data, not a performance number.  The plan gates its
# two largest phases on it: SX4 (incremental simplex) is worth starting only if
# REFINE_MAX_LA_CONSTR actually bites, and SX6 (boolean structure beyond small
# DNF) only if the cube caps do.  See docs/upcoming/solver-extension-plan.md.
#
# Populations:
#   1. the SMT-LIB corpus      (tests/corpus/smtlib, via tur_refine_corpus)
#   2. in-tree refinement users (tests/fixtures/*refine*, *gadt*, via tur check)
#   3. fuzzer-generated VCs     (tests/refine-fuzz-src.py's own generator)
#
# Usage:
#   bash benchmarks/run-cap-sweep.sh              # all three populations
#   CAP_SWEEP_FUZZ_N=500 bash benchmarks/run-cap-sweep.sh
#
# Env:
#   CAP_SWEEP_FUZZ_N     fuzzer programs to generate  (default 200)
#   CAP_SWEEP_FUZZ_SEED  fuzzer seed                  (default 1)
#   CAP_SWEEP_OUT        results file                 (default benchmarks/cap-sweep-results.md)

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="./build/tur"
CORPUS_BIN="./build/tur_refine_corpus"
OUT="${CAP_SWEEP_OUT:-benchmarks/cap-sweep-results.md}"
FUZZ_N="${CAP_SWEEP_FUZZ_N:-200}"
FUZZ_SEED="${CAP_SWEEP_FUZZ_SEED:-1}"

[ -x "$TUR" ]        || { echo "cap-sweep: $TUR not built" >&2; exit 2; }
[ -x "$CORPUS_BIN" ] || { echo "cap-sweep: $CORPUS_BIN not built (cmake --build build)" >&2; exit 2; }

# The suite has been bitten by a rebuild landing mid-run: fixtures exec the
# compiler straight out of the build tree, so a concurrent `cmake --build`
# swaps it underneath the sweep and the rows describe two different binaries.
# Stamp it, re-check at the end, and refuse to publish if it moved.
stamp() { ls -l --time-style=+%s "$1" 2>/dev/null | awk '{print $5, $6}'; }
TUR_STAMP="$(stamp "$TUR")"
CORPUS_STAMP="$(stamp "$CORPUS_BIN")"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The interpreter path leaks its process-lifetime closures by design; this
# sweep measures the compiler's solver, so leak-checking the spawned work is
# not what it is for.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

echo "cap-sweep: 1/3 SMT-LIB corpus"
TUR_CORPUS_CAPS=1 "$CORPUS_BIN" tests/corpus/smtlib > "$WORK/corpus.log" 2>&1
CORPUS_RC=$?

echo "cap-sweep: 2/3 in-tree refinement fixtures"
: > "$WORK/fixtures.log"
for d in tests/fixtures/*refine* tests/fixtures/*gadt*; do
    [ -d "$d" ] || continue
    i="$d/input.tur"
    [ -f "$i" ] || i="$d/$(basename "$d").tur"
    [ -f "$i" ] || continue
    TUR_REFINE_STATS=1 timeout 60 "$TUR" check "$i" 2>&1 \
        | sed -n "s|^refine:   |$(basename "$d")\t|p" >> "$WORK/fixtures.log"
done

echo "cap-sweep: 3/3 fuzzer-generated VCs (n=$FUZZ_N seed=$FUZZ_SEED)"
# Reuse the fuzzer's OWN generator rather than a second one written here: the
# point of this population is that it is the same distribution the differential
# fuzzer already explores, so a private generator would measure the wrong thing.
mkdir -p "$WORK/fuzz"
python3 - "$WORK/fuzz" "$FUZZ_N" "$FUZZ_SEED" <<'PY'
import importlib.util, os, random, sys
out, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
spec = importlib.util.spec_from_file_location("rfs", "tests/refine-fuzz-src.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
for i in range(n):
    rng = random.Random(seed * 1000003 + i)
    with open(os.path.join(out, "c%06d.tur" % i), "w") as f:
        f.write(m.Gen(rng, "both").program())
PY
: > "$WORK/fuzz.log"
for f in "$WORK"/fuzz/*.tur; do
    TUR_REFINE_STATS=1 timeout 60 "$TUR" check "$f" 2>&1 \
        | sed -n "s|^refine:   |$(basename "$f")\t|p" >> "$WORK/fuzz.log"
done

if [ "$(stamp "$TUR")" != "$TUR_STAMP" ] || [ "$(stamp "$CORPUS_BIN")" != "$CORPUS_STAMP" ]; then
    echo "cap-sweep: WARNING -- a binary changed while this sweep was in progress;" >&2
    echo "cap-sweep: the rows would describe two different compilers.  Not publishing." >&2
    exit 2
fi

python3 - "$WORK" "$OUT" "$FUZZ_N" "$FUZZ_SEED" "$CORPUS_RC" <<'PY'
import re, sys, collections, subprocess, datetime

work, out, fuzz_n, fuzz_seed, corpus_rc = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]

# One row per cap.  `limit` is filled from whichever source names it -- the
# corpus emitter does not print limits, the per-compile summary does.
CAPS = ["cubes", "cube_lits", "expand_depth", "la_vars", "la_constr",
        "euf_terms", "no_shared", "path_hyps", "model_vars"]
PRETTY = {"cubes": "cubes", "cube literals": "cube_lits",
          "expand depth": "expand_depth", "LA vars": "la_vars",
          "LA constraints": "la_constr", "EUF terms": "euf_terms",
          "NO shared": "no_shared", "path hyps": "path_hyps",
          "model vars": "model_vars"}

# Caps the SMT-LIB corpus population cannot exercise at all.  Reporting a flat
# 0 for them would read as "measured, never bit" when the truth is "this
# population cannot reach it", so the corpus table says n/a and why.
COMPILED_ONLY = {
    "path_hyps":  "this population does not elaborate",
    "model_vars": "this harness does not run the model search",
}

# Caps whose peak saturates at the limit because the producer stops at it.
# Their headroom column is only meaningful while hits is 0.
SATURATING = {"path_hyps"}

def blank():
    return {c: {"hits": 0, "peak": 0, "worst": "", "limit": 0} for c in CAPS} | \
           {"la_fm": {"hits": 0}, "no_rounds": {"hits": 0}, "model_run": {"hits": 0}}

def note(acc, cap, hits, peak, limit, who):
    r = acc[cap]
    r["hits"] += hits
    if limit: r["limit"] = limit
    if peak > r["peak"]: r["peak"], r["worst"] = peak, who

# -- population 1: the corpus emitter's machine-readable line ----------------
corpus = blank()
capped_units = collections.Counter()
n_corpus = 0
for line in open(work + "/corpus.log"):
    if not line.startswith("  caps "): continue
    n_corpus += 1
    who = line.split()[1].split("/")[-1]
    d = dict(re.findall(r"(\w+)=([\d:]+)", line))
    fired = False
    for k, v in d.items():
        parts = v.split(":")
        h = int(parts[0]); p = int(parts[1]) if len(parts) > 1 else 0
        if h: fired = True
        if k in CAPS:                       note(corpus, k, h, p, 0, who)
        elif k in ("la_fm", "no_rounds"):   corpus[k]["hits"] += h
    if fired: capped_units["corpus"] += 1

# -- populations 2 and 3: the per-compile summary ---------------------------
def parse_summary(path, acc, tag):
    seen = set()
    for line in open(path):
        who, rest = line.split("\t", 1)
        rest = rest.strip()
        m = re.match(r"(.+?)\s+peak\s+(\d+)\s*/\s*(\d+)\s*(\*\* HIT)?$", rest)
        if m:
            cap = PRETTY.get(m.group(1).strip())
            if not cap: continue
            hit = bool(m.group(4))
            note(acc, cap, 1 if hit else 0, int(m.group(2)), int(m.group(3)), who)
            if hit: seen.add(who)
            continue
        m = re.match(r"model vars run\s+(\d+)", rest)
        if m:
            # NOT a cap hit of its own -- it is the SUBSET of `model vars` hits a
            # higher cap would actually help, so it must never add to the capped
            # unit count or a unit would be tallied twice.
            acc["model_run"]["hits"] += int(m.group(1))
            continue
        m = re.match(r"(FM blow-ups|NO rounds out)\s+(\d+)", rest)
        if m and int(m.group(2)):
            acc["la_fm" if m.group(1).startswith("FM") else "no_rounds"]["hits"] += int(m.group(2))
            seen.add(who)
    capped_units[tag] = len(seen)
    return acc

fixtures = parse_summary(work + "/fixtures.log", blank(), "fixtures")
fuzz     = parse_summary(work + "/fuzz.log",     blank(), "fuzz")

# The corpus emitter prints no limits; borrow them from the summary parse.
for c in CAPS:
    if not corpus[c]["limit"]:
        corpus[c]["limit"] = fixtures[c]["limit"] or fuzz[c]["limit"]

def n_units(path):
    return len({l.split("\t")[0] for l in open(path) if "\t" in l})

pops = [("SMT-LIB corpus", corpus, n_corpus, "corpus"),
        ("in-tree fixtures", fixtures, n_units(work + "/fixtures.log"), "fixtures"),
        ("fuzzer VCs", fuzz, n_units(work + "/fuzz.log"), "fuzz")]

try:
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip()
except Exception:
    rev = "?"

L = []
L.append("# Refinement solver cap sweep (SX0(b))\n")
L.append("Generated: %s at %s by `benchmarks/run-cap-sweep.sh`.\n"
         % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), rev))
L.append("Every cap below degrades to `RT_UNKNOWN` -> the runtime check survives, so a\n"
         "hit is never unsound -- it is lost completeness, and only lost completeness that\n"
         "costs a real proof matters.  `peak` is the high-water mark of the quantity the\n"
         "cap bounds, recorded on every query rather than only on the ones that overflow,\n"
         "so a cap that never fires still reports its headroom.\n")
L.append("`model_vars` bounds the counterexample SEARCH, so a hit there costs a\n"
         "refutation rather than a proof: the obligation stays `unknown` instead of\n"
         "reporting the counterexample it has.  Its `would run` row is the subset a\n"
         "higher cap would actually help -- a VC over the cap may also carry a non-int\n"
         "variable, which the sort gate declines at any limit.  Argue a raise from that\n"
         "row, never from the hits.\n")
L.append("**Do not read the fuzzer population's `model_vars` headroom as a signal.**\n"
         "`tests/refine-fuzz-src.py` generates at most TWO parameters per program\n"
         "(`rng.randint(0, 2)` / `randint(1, 2)` in its shape methods), so a generated\n"
         "VC structurally cannot exceed those plus `r`.  A peak that sits exactly on the\n"
         "limit there is the generator's ceiling, not evidence about real code, and 0\n"
         "hits from that population is not evidence the cap never bites.  A four-\n"
         "parameter function with a refined return trips it immediately -- which is an\n"
         "ordinary shape none of the three swept populations happens to contain.\n")
L.append("`path_hyps` is a COLLECTION cap (RT_CS_PATH_MAX_HYPS -- the branch guards\n"
         "recovered for a call-site crossing), not a solver stage.  It reads `n/a` for the\n"
         "SMT-LIB corpus because that population feeds VCs straight to the chain and never\n"
         "elaborates anything, and its peak SATURATES at the limit, so the headroom column\n"
         "means something only while its hits are 0.\n")
for name, acc, n, tag in pops:
    L.append("\n## %s -- %d unit(s), %d with a cap hit\n" % (name, n, capped_units[tag]))
    L.append("| cap | hits | peak | limit | headroom | worst unit |")
    L.append("|---|---:|---:|---:|---:|---|")
    for c in CAPS:
        if c in COMPILED_ONLY and tag == "corpus":
            L.append("| %s | n/a | n/a | %d | n/a | %s |"
                     % (c, acc[c]["limit"], COMPILED_ONLY[c]))
            continue
        r = acc[c]
        lim = r["limit"]
        # A peak ABOVE the limit is not negative headroom, it is the count of
        # eligible items the cap turned away -- render it as such rather than
        # as a minus sign the reader has to decode.
        if not lim:                head = "-"
        elif r["peak"] > lim:      head = "OVER by %d" % (r["peak"] - lim)
        else:                      head = "%d%%" % round(100.0 * (lim - r["peak"]) / lim)
        # A saturating peak cannot report headroom once it has bitten: the
        # producer stopped at the limit, so the peak reads the limit whatever
        # the real demand was.
        if c in SATURATING and r["hits"]: head = "saturated -- see hits"
        L.append("| %s | %d | %d | %d | %s | %s |"
                 % (c, r["hits"], r["peak"], lim, head, r["worst"] or "-"))
    L.append("| la_fm (FM blow-up) | %d | - | - | - | - |" % acc["la_fm"]["hits"])
    L.append("| no_rounds (exchange budget) | %d | - | - | - | - |" % acc["no_rounds"]["hits"])
    if tag == "corpus":
        L.append("| model_vars would run | n/a | - | - | - | %s |" % COMPILED_ONLY["model_vars"])
    else:
        L.append("| model_vars would run | %d | - | - | - | of the %d over the cap |"
                 % (acc["model_run"]["hits"], acc["model_vars"]["hits"]))
L.append("\nCorpus harness exit code: %s (0 = PASS, no soundness failure).\n" % corpus_rc)
L.append("Fuzzer population: n=%s seed=%s, generated by `tests/refine-fuzz-src.py`'s own `Gen`.\n"
         % (fuzz_n, fuzz_seed))

open(out, "w").write("\n".join(L) + "\n")
print("cap-sweep: wrote %s" % out)
for name, acc, n, tag in pops:
    print("cap-sweep:   %-18s %d unit(s), %d capped" % (name, n, capped_units[tag]))
PY
