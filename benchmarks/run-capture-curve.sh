#!/usr/bin/env bash
# benchmarks/run-capture-curve.sh -- SX0(a): the capture/restore cost curve.
#
# Runs benchmarks/bench-capture-restore.tur, fits `a + b*F + c*E` per capture
# path, and writes benchmarks/capture-curve-results.md plus the raw CSV.
#
# The number this exists to produce is not "how far off native" -- it is cost
# per capture and per restore as a function of live state under the prompt, and
# specifically the INTERCEPT against the SLOPE.  A strategy with a large
# constant and no slope beats one with a small constant and a real slope past
# some crossover, and where that crossover sits relative to real workloads is
# what decides the SX1 trail design.  The report ends with that crossover.
#
# See docs/upcoming/solver-extension-plan.md sections 4.2-4.3 and SX0(a).
#
# Usage:
#   bash benchmarks/run-capture-curve.sh
#
# Env:
#   CAPTURE_CURVE_OUT   report path  (default benchmarks/capture-curve-results.md)
#   CAPTURE_CURVE_CSV   raw CSV path (default benchmarks/capture-curve.csv)
#   CAPTURE_CURVE_ALLOW_SANITIZED=1  measure with a sanitized build anyway

set -uo pipefail
cd "$(dirname "$0")/.."

BENCH=benchmarks/bench-capture-restore.tur
OUT="${CAPTURE_CURVE_OUT:-benchmarks/capture-curve-results.md}"
CSV="${CAPTURE_CURVE_CSV:-benchmarks/capture-curve.csv}"

# ---------------------------------------------------------------------------
# Pick a compiler whose output is worth timing.
#
# The Debug build compiles tur with -fsanitize=address,undefined and the
# emitted program links the sanitized libturi, so every row would be measuring
# ASan's shadow-memory bookkeeping rather than dk_copy_node.  A benchmark that
# quietly reports sanitizer overhead as a design input is worse than no
# benchmark, so this refuses rather than warns.
# ---------------------------------------------------------------------------
TUR=""
for cand in ./build-release/tur ./build-nosan/tur; do
    [ -x "$cand" ] && { TUR="$cand"; break; }
done
if [ -z "$TUR" ]; then
    if [ -x ./build/tur ] && [ "${CAPTURE_CURVE_ALLOW_SANITIZED:-0}" = 1 ]; then
        TUR=./build/tur
        echo "capture-curve: WARNING -- measuring with a sanitized build." >&2
        echo "capture-curve: these rows are for eyeballing shapes; do not check them in." >&2
    else
        echo "capture-curve: no unsanitized compiler found." >&2
        echo "  The Debug build's numbers would be sanitizer overhead, not mechanism cost." >&2
        echo "  Build one without:" >&2
        echo "    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5" >&2
        echo "    cmake --build build-release -j" >&2
        echo "  Or set CAPTURE_CURVE_ALLOW_SANITIZED=1 to override." >&2
        exit 2
    fi
fi
echo "capture-curve: compiler $TUR"

# The suite has been bitten by a rebuild landing mid-run.  Stamp the compiler
# and refuse to publish if it moved: rows measured against two different
# binaries are not one curve.
stamp() { ls -l --time-style=+%s "$1" 2>/dev/null | awk '{print $5, $6}'; }
TUR_STAMP="$(stamp "$TUR")"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "capture-curve: building $BENCH"
"$TUR" build "$BENCH" -o "$WORK/bench" 2>"$WORK/build.log" || {
    echo "capture-curve: build failed" >&2; tail -20 "$WORK/build.log" >&2; exit 2; }

# ---------------------------------------------------------------------------
# The interpreter leg.
#
# The plan asks for a compiled row and an --interpret row.  Ask for it, and
# record what actually comes back: every timed region in the benchmark is
# inline C, so the tree-walker declines the file outright.  That is a real
# answer about this benchmark, not a gap to paper over -- see the report's
# notes for what would answer the interpreter question instead.
# ---------------------------------------------------------------------------
INTERP_NOTE="$("$TUR" --interpret "$BENCH" 2>&1 | head -1)"

echo "capture-curve: running (quiet the machine -- this measures nanoseconds)"
"$WORK/bench" > "$WORK/raw.csv" 2>"$WORK/run.log" || {
    echo "capture-curve: benchmark failed" >&2; tail -20 "$WORK/run.log" >&2; exit 2; }

if [ "$(stamp "$TUR")" != "$TUR_STAMP" ]; then
    echo "capture-curve: WARNING -- the compiler changed while this run was in progress." >&2
    echo "capture-curve: the rows would describe two different binaries.  Not publishing." >&2
    exit 2
fi

cp "$WORK/raw.csv" "$CSV"

python3 - "$WORK/raw.csv" "$OUT" "$TUR" "$INTERP_NOTE" <<'PY'
import sys, csv, subprocess, datetime, platform, os

raw, out, tur, interp_note = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

rows, notes = [], []
for line in open(raw):
    line = line.rstrip("\n")
    if line.startswith("#"): notes.append(line[1:].strip()); continue
    rows.append(line)
rd = list(csv.DictReader(rows))

def cap_ns(r): return float(r["cap_ns_total"]) / float(r["cap_iters"])
def res_ns(r): return float(r["res_ns_total"]) / float(r["res_iters"])

def ols(pts):
    """Weighted least squares for y = a + b*x1 + c*x2, returning (a, b, c, r2).

    Weights are 1/y^2 -- minimizing RELATIVE rather than absolute error.  That
    is not a smoothing trick, it is the right estimator for this data twice
    over: the noise here is multiplicative (a scheduler blip costs a
    percentage, not a fixed number of nanoseconds), and F is swept
    logarithmically over four decades, so plain OLS is decided almost entirely
    by the F=4096 row.  Unweighted, the fit reported an intercept of -851 ns
    with R^2 = 0.992 -- a great-looking fit whose prediction at F=8 was
    negative, against a measured 186 ns.  The plan asks for the intercept and
    the slope as SEPARATELY actionable numbers, and an estimator that gets the
    slope right by throwing the intercept away does not deliver that.

    Spelled out rather than pulled from numpy: this has to run wherever the
    benchmark does, and three normal equations are not worth a dependency.
    Degrades to a two-parameter fit when x2 never varies."""
    pts = [p for p in pts if p[0] > 0]
    n = len(pts)
    if n < 3: return None
    w = [1.0/(p[0]*p[0]) for p in pts]
    def S(f): return sum(wi*f(p) for wi, p in zip(w, pts))
    if len({p[2] for p in pts}) <= 1:
        sw, swx, swy = S(lambda p: 1.0), S(lambda p: p[1]), S(lambda p: p[0])
        swxx, swxy = S(lambda p: p[1]*p[1]), S(lambda p: p[1]*p[0])
        d = sw*swxx - swx*swx
        if abs(d) < 1e-30: return None
        b = (sw*swxy - swx*swy)/d; a = (swy - b*swx)/sw; c = 0.0
    else:
        M = [[S(lambda p: 1.0),      S(lambda p: p[1]),      S(lambda p: p[2]),      S(lambda p: p[0])],
             [S(lambda p: p[1]),     S(lambda p: p[1]*p[1]), S(lambda p: p[1]*p[2]), S(lambda p: p[1]*p[0])],
             [S(lambda p: p[2]),     S(lambda p: p[2]*p[1]), S(lambda p: p[2]*p[2]), S(lambda p: p[2]*p[0])]]
        for i in range(3):
            pv = max(range(i,3), key=lambda r: abs(M[r][i]))
            if abs(M[pv][i]) < 1e-30: return None
            M[i], M[pv] = M[pv], M[i]
            for r in range(3):
                if r == i: continue
                f = M[r][i]/M[i][i]
                for cc in range(i,4): M[r][cc] -= f*M[i][cc]
        a, b, c = (M[i][3]/M[i][i] for i in range(3))
    # R^2 on the same weighted footing, so it describes the fit that was made.
    sw = sum(w)
    ybar = sum(wi*p[0] for wi, p in zip(w, pts))/sw
    ss_t = sum(wi*(p[0]-ybar)**2 for wi, p in zip(w, pts))
    ss_r = sum(wi*(p[0]-(a+b*p[1]+c*p[2]))**2 for wi, p in zip(w, pts))
    r2 = 1.0 - ss_r/ss_t if ss_t > 0 else 1.0
    return (a, b, c, r2)

# The env-clone term is PER FRAME on the chain path -- dk_copy_node fires
# env_clone once per owning node -- so the second regressor is F*E, not E.
# Fitting the plan's literal `a + b*F + c*E` drove the intercept to -3458 ns,
# which is not a constant any mechanism can have: with no F*E term available
# the fit absorbed 4096 frames' worth of 64-byte copies into b and paid for it
# in a.  The cloneable path holds exactly one env, so there F*E collapses to E
# and the two spellings agree.
def regressors(path, r):
    F, E = float(r["F"]), float(r["E"])
    return (F, F*E) if path == "dk" else (F, E)

L = []
L.append("# Capture/restore cost curve (SX0(a))\n")
try:
    rev = subprocess.run(["git","rev-parse","--short","HEAD"],capture_output=True,text=True).stdout.strip()
except Exception:
    rev = "?"
cpu = "?"
try:
    for line in open("/proc/cpuinfo"):
        if line.startswith("model name"): cpu = line.split(":",1)[1].strip(); break
except Exception:
    pass
L.append("Generated: %s at %s by `benchmarks/run-capture-curve.sh`.\n"
         % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), rev))
L.append("Compiler: `%s`.  Host: %s, %s, %s core(s).\n" % (tur, platform.machine(), cpu, os.cpu_count()))
L.append("""
Cost per capture and per restore as a function of live state under the prompt.
The useful reading is the **intercept against the slope**: a strategy with a
large constant and no slope beats one with a small constant and a real slope
past some crossover, and that crossover is the design input. Every figure is
nanoseconds per operation, best of three timed rounds after an untimed warm-up.

Axes: `F` frames under the prompt, `E` owning-env bytes per frame (`rc` = a
refcount bump rather than a byte copy), `R` resumes per capture, `T` trailed
writes under the prompt (0 until SX1 exists).
""")

L.append("\n## Fitted models\n")
L.append("`ns = a + b*F + c*(F*E)` for the chain path, `a + b*F + c*E` for the rest.")
L.append("The env term is per-FRAME on a chain -- `dk_copy_node` fires `env_clone` once")
L.append("per owning node -- so `F*E` is the regressor; the script records what fitting")
L.append("the plan's literal `c*E` did instead. `rc` rows are excluded from the `c` fit")
L.append("(a refcount bump is a different mechanism, not one byte of a copy) and appear")
L.append("in the per-path tables below.\n")
L.append("| path | R | what | a (ns) | b (ns/frame) | c (ns/env byte/frame) | R^2 |")
L.append("|---|---:|---|---:|---:|---:|---:|")
paths = ["dk", "fiber", "cloneable"]
for path in ("dk", "fiber"):
    for R in ("1", "8"):
        sel = [r for r in rd if r["path"]==path and r["R"]==R and float(r["E"])>=0]
        if not sel: continue
        for label, fn in (("capture", cap_ns), ("restore", res_ns)):
            pts = [(fn(r),) + regressors(path, r) for r in sel]
            f = ols(pts)
            if not f: continue
            L.append("| %s | %s | %s | %.1f | %.3f | %.4f | %.4f |"
                     % (path, R, label, f[0], f[1], f[2], f[3]))
L.append("")
L.append("A near-zero R^2 on the `fiber` rows is the RESULT, not a bad fit: there is no")
L.append("trend in F for the regression to explain. Read those rows as \"a = the cost,")
L.append("b = 0\" -- which is precisely the constant-cost, zero-slope strategy the")
L.append("literature describes, measured rather than assumed.\n")

# The cloneable path has no chain, so F is constant at 0 and a fit against it is
# undefined.  Fit against E, which is the only thing that can move it.
L.append("`cloneable` is fitted separately, against E alone: it holds one env and no")
L.append("chain, so F does not enter and `b` would be a regression against a constant.")
L.append("Its slope in F is zero BY CONSTRUCTION, not by measurement.\n")
L.append("| path | R | what | a (ns) | c (ns/env byte) | R^2 |")
L.append("|---|---:|---|---:|---:|---:|")
for R in ("1", "8"):
    sel = [r for r in rd if r["path"]=="cloneable" and r["R"]==R and float(r["E"])>=0]
    if not sel: continue
    for label, fn in (("capture", cap_ns), ("restore", res_ns)):
        f = ols([(fn(r), float(r["E"]), 0.0) for r in sel])
        if not f: continue
        L.append("| cloneable | %s | %s | %.1f | %.4f | %.4f |" % (R, label, f[0], f[1], f[3]))

L.append("\n## Per-frame cost across the sweep (dk, E=0, R=1)\n")
L.append("A single slope assumes the per-frame cost is constant. This column is how far")
L.append("that assumption holds -- and where it stops. The cost settles into a flat")
L.append("band from F=32 to F=2048 and then steps up sharply at F=4096: a chain that")
L.append("size no longer fits in cache, and both copying and replaying it start paying")
L.append("for misses. The fitted `b` is the flat band; past ~2048 frames the real cost")
L.append("is worse than the model says.\n")
L.append("| F | capture ns/frame | restore ns/frame |")
L.append("|---:|---:|---:|")
for r in rd:
    if r["path"]=="dk" and r["E"]=="0" and r["R"]=="1" and float(r["F"]) > 0:
        F = float(r["F"])
        L.append("| %s | %.2f | %.2f |" % (r["F"], cap_ns(r)/F, res_ns(r)/F))

for path in paths:
    sel = [r for r in rd if r["path"]==path]
    if not sel: continue
    L.append("\n## %s\n" % path)
    L.append("| F | E | R | capture ns | restore ns | bytes/capture |")
    L.append("|---:|---:|---:|---:|---:|---:|")
    for r in sel:
        e = "rc" if float(r["E"]) < 0 else r["E"]
        L.append("| %s | %s | %s | %.1f | %.2f | %s |"
                 % (r["F"], e, r["R"], cap_ns(r), res_ns(r), r["bytes_per_capture"]))

L.append("\n## Baselines\n")
L.append("| baseline | ns/op |")
L.append("|---|---:|")
for r in rd:
    if r["path"].startswith("baseline"):
        L.append("| %s | %.2f |" % (r["path"].replace("baseline-",""), cap_ns(r)))

L.append("\n## The crossover\n")
L.append("Fitted on the `E = 0` rows only -- no owning envs on either side, so this is")
L.append("the two mechanisms compared and nothing else.\n")
def fit_e0(path, R, fn):
    sel = [r for r in rd if r["path"]==path and r["R"]==R and r["E"]=="0" and float(r["F"])>0]
    return ols([(fn(r), float(r["F"]), 0.0) for r in sel]) if len(sel) >= 3 else None
lines = []
for label, fn in (("capture", cap_ns), ("restore", res_ns)):
    d, f = fit_e0("dk","1",fn), fit_e0("fiber","1",fn)
    if d and f and d[1] > 1e-9:
        x = (f[0] - d[0]) / d[1]
        lines.append("- **%s**: the DK chain costs `%.0f + %.1f*F` ns (R^2 %.3f); a fiber costs "
                     "a flat `%.0f` ns (measured slope %.3f ns/frame, R^2 %.3f). They cross at "
                     "**F = %.0f frames** -- below that the chain slice is cheaper, above it "
                     "the fiber's constant-cost switch wins."
                     % (label, d[0], d[1], d[3], f[0], f[1], f[3], x))
L.extend(lines if lines else ["- (not enough points to fit)"])

L.append("\n## Notes\n")
L.append("- **Interpreter leg:** requested and declined. `tur --interpret` on this")
L.append("  benchmark reports:\n")
L.append("  > %s\n" % interp_note)
L.append("  Every timed region here is inline C, by design -- the question is what the")
L.append("  runtime mechanism costs, not what a driver costs to reach it -- so the")
L.append("  tree-walker has nothing it can run. The plan's interpreter question (is the")
L.append("  tree-walker adequate for a thousands-per-second search layer?) needs a")
L.append("  Turmeric-level search benchmark instead; SX2's `bench-logic-query` and")
L.append("  `bench-backtrack-n-queens` are exactly that, and are where that row belongs.")
for n in notes:
    L.append("- Run metadata: `%s`" % n)

open(out,"w").write("\n".join(L) + "\n")
print("capture-curve: wrote %s" % out)
for l in lines: print("capture-curve: %s" % l.replace("**","").replace("`",""))
PY
echo "capture-curve: raw CSV at $CSV"
