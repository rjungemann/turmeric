#!/usr/bin/env python3
"""tests/generic-spec-matrix.py -- a value of type `A` inside a generic body,
every way in, every way out, at every representation.

Why this exists
---------------
A generic defn is emitted twice over: once as the carrier BASE (every `A` is
the int64 word) and once per instantiation as a SPEC (every `A` is the concrete
C type -- `double`, `float`, `bool`, `const char *`, a by-value struct).  Inside
a spec, each expression typed `A` must therefore be the concrete value, and
each crossing into something still generic -- a stdlib accessor, a generic
struct's base constructor, a lambda emitted once on the carrier ABI -- must
convert between the two at the BITS level.  Every consumer and every producer
is its own emit path, and each one that forgets is a silent wrong answer:
7.1's bit pattern printed as 4.61968e+18, a float32 printed as 1.088632e+09 or
0, a cstr word dereferenced as a pointer to a cstr.

The fixture corpus held almost none of these shapes -- the float-conversion
lint (tests/check-emitted-float-conversions.py) found ZERO findings on 2512
fixture programs while `(defn first-of [A] [v : (Vec A)] : A (vec-get v 0))`
at `float` was wrong.  A detector sees only the programs that exist, so this
harness makes them exist: a deterministic PRODUCER x SINK x TYPE matrix, each
cell a whole program whose correct stdout is known.

Every cell is checked three ways:

  * compiled stdout equals the expected value,
  * `tur --interpret` stdout equals it too (the two paths must agree),
  * the emitted C has no float<->int VALUE conversion (fconv_lint) -- which
    flags a wrong crossing even where the printed digits happen to survive.

It is exhaustive rather than random because the space is small and each
cell is a regression test: a fixed cell stays fixed, a new producer or sink
is a new row, and `--baseline` ratchets the open cells so the harness can gate
before every cell is green.

Usage
-----
    python3 tests/generic-spec-matrix.py [--jobs N] [--tur ./build/tur]
        [--only PRODUCER/SINK/TYPE glob] [--no-interp] [--no-lint]
        [--baseline FILE] [--write-baseline FILE] [--keep DIR]

Exit 1 if a cell fails that the baseline does not list (or any cell fails
when no baseline is given), 0 otherwise.  A baseline cell that now PASSES is
reported as FIXED so the baseline can only shrink.
"""

import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fconv_lint  # noqa: E402

# -- types -------------------------------------------------------------------
# name -> (spelling, literal, render, expected stdout line)
# `render` turns the call result into something `println` prints directly.
TYPES = {
    "int":     ("int", "42", "%s", "42"),
    "float":   ("float", "7.1", "%s", "7.1"),
    "float32": ("float32", "(:: 7.1 float32)", "%s", "7.1"),
    "bool":    ("bool", "true", "%s", "true"),
    "cstr":    ("cstr", '"hi"', "%s", "hi"),
    "int8":    ("int8", "(:: 7 int8)", "%s", "7"),
    "int32":   ("int32", "(:: 70000 int32)", "%s", "70000"),
    "struct":  ("MxP", "(MxP 3 4.5)", "(.y %s)", "4.5"),
    "heap":    ("MxH", "(MxH 5 2.25)", "(.y %s)", "2.25"),
    "opt":     ("(Option float)", "(some 2.5)", "(unwrap-or %s 0.5)", "2.5"),
}

PRELUDE = """\
(defstruct MxP [x : int y : float])
(defstruct MxH :heap [x : int y : float])
(defstruct MxBox [A] [val : A])
(defn mx-id [A] [y : A] : A y)
(defn mx-true [] : bool true)
"""

# -- producers ---------------------------------------------------------------
# name -> (generic params after [A], A-typed expression, call args builder)
# The generic function is `(defn mx [A] [PARAMS] : A SINK(EXPR))`; main calls
# `(mx ARGS)` with the literal L of the concrete type T.
PRODUCERS = {
    "param":    ("x : A", "x",
                 lambda L, T: L),
    "ident":    ("x : A", "(mx-id x)",
                 lambda L, T: L),
    "vecget":   ("v : (Vec A)", "(vec-get v 0)",
                 lambda L, T: "(let [v (:: (vec-new) (Vec %s))] (vec-push! v %s) v)"
                 % (T, L)),
    "boxfield": ("b : (MxBox A)", "(.val b)",
                 lambda L, T: "(MxBox %s)" % L),
    "optmatch": ("o : (Option A) d : A", "(match o (Some q) q (None) d)",
                 lambda L, T: "(some %s) %s" % (L, L)),
    "unwrapor": ("o : (Option A) d : A", "(unwrap-or o d)",
                 lambda L, T: "(some %s) %s" % (L, L)),
    "okval":    ("r : (Result A int)", "(ok-val r)",
                 lambda L, T: "(:: (ok %s) (Result %s int))" % (L, T)),
    "pairfst":  ("p : (Pair A int)", "(pair-fst p)",
                 lambda L, T: "(pair %s 0)" % L),
    "thunk":    ("f : (fn [] A)", "(f)",
                 lambda L, T: "(fn [] %s)" % L),
}

# -- sinks -------------------------------------------------------------------
# name -> template over E (an A-typed expression); result must be A.
SINKS = {
    "tail":    "{E}",
    "let":     "(let [y {E}] y)",
    "ident":   "(mx-id {E})",
    "if":      "(if (mx-true) {E} {E})",
    "do":      "(do (mx-true) {E})",
    "box":     "(.val (MxBox {E}))",
    "boxlet":  "(let [b (MxBox {E})] (.val b))",
    "some":    "(match (some {E}) (Some q) q (None) {E})",
    "vec":     "(let [w (vec-new)] (vec-push! w {E}) (vec-get w 0))",
    "pair":    "(pair-fst (pair {E} 0))",
    "lambda":  "((fn [z : A] : A z) {E})",
    "capture": "(let [c {E}] ((fn [] c)))",
    "mut":     "(let [^mut c {E}] (set! c {E}) c)",
}


def gen(producer, sink, tyname):
    params, expr, argsf = PRODUCERS[producer]
    T, L, render, want = TYPES[tyname]
    body = SINKS[sink].format(E=expr)
    call = "(mx %s)" % argsf(L, T)
    src = (";; generic-spec-matrix cell %s/%s/%s\n" % (producer, sink, tyname)
           + PRELUDE
           + "(defn mx [A] [%s] : A\n  %s)\n" % (params, body)
           + "(defn main [] : int\n  (println %s)\n  0)\n" % (render % call))
    return src, want


def run(cmd, env, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           cwd=REPO, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -999, "", "timeout"


def one(cell, args, workdir, clang):
    producer, sink, tyname = cell
    src, want = gen(producer, sink, tyname)
    stem = "%s__%s__%s" % cell
    path = os.path.join(workdir, stem + ".tur")
    with open(path, "w") as f:
        f.write(src)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    env.pop("TUR_STDLIB_DIR", None)
    res = {"cell": cell, "problems": []}

    rc, out, err = run([args.tur, "check", path], env)
    if rc != 0:
        res["problems"].append("REJECT: " + (err.strip().splitlines() or ["?"])[0][:160])
        return res

    rc, out, err = run([args.tur, "run", path], env)
    got = out.strip().splitlines()[-1] if out.strip() else ""
    if rc != 0:
        tail = (err.strip().splitlines() or ["?"])
        msg = next((l for l in tail if "error" in l), tail[-1])
        res["problems"].append("compiled rc=%d: %s" % (rc, msg.strip()[:160]))
    elif got != want:
        res["problems"].append("compiled printed %r, want %r" % (got, want))

    if not args.no_interp:
        rc, out, err = run([args.tur, "--interpret", path], env)
        got = out.strip().splitlines()[-1] if out.strip() else ""
        if rc != 0:
            tail = [l for l in err.strip().splitlines() if "makecontext" not in l]
            res["problems"].append("interp rc=%d: %s" % (rc, (tail or ["?"])[-1].strip()[:160]))
        elif got != want:
            res["problems"].append("interp printed %r, want %r" % (got, want))

    if clang and not args.no_lint:
        rc, out, err = run([args.tur, "emit-c", path], env)
        if rc == 0 and fconv_lint.MARK in out:
            cfile = os.path.join(workdir, stem + ".c")
            with open(cfile, "w") as f:
                f.write(out)
            for (line, kind, how, text) in fconv_lint.lint_c(cfile, clang):
                res["problems"].append("lint %s %s: %s" % (how, kind, text[:120]))
            if not args.keep:
                os.unlink(cfile)
    if not args.keep and not res["problems"]:
        os.unlink(path)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--only", default="*", help="glob over producer/sink/type")
    ap.add_argument("--no-interp", action="store_true")
    ap.add_argument("--no-lint", action="store_true")
    ap.add_argument("--baseline", help="file of known-open cells, one per line")
    ap.add_argument("--write-baseline", help="write the failing cells here")
    ap.add_argument("--keep", help="keep generated programs in this directory")
    args = ap.parse_args()

    clang = None if args.no_lint else fconv_lint.find_clang()
    if not args.no_lint and not clang:
        print("generic-spec-matrix: lint UNAVAILABLE -- no clang; float<->int "
              "value conversions are NOT being checked")

    cells = [(p, s, t) for p in PRODUCERS for s in SINKS for t in TYPES
             if fnmatch.fnmatch("%s/%s/%s" % (p, s, t), args.only)]
    workdir = args.keep or tempfile.mkdtemp(prefix="gsm-")
    os.makedirs(workdir, exist_ok=True)
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda c: one(c, args, workdir, clang), cells))
    finally:
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)

    known = set()
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline) as f:
            known = {l.strip() for l in f if l.strip() and not l.startswith("#")}

    failing, new, fixed = [], [], []
    for r in results:
        key = "%s/%s/%s" % r["cell"]
        if r["problems"]:
            failing.append(key)
            tag = "KNOWN" if key in known else "FAIL"
            if key not in known:
                new.append(key)
            for pr in r["problems"]:
                print("%s %s -- %s" % (tag, key, pr))
        elif key in known:
            fixed.append(key)
            print("FIXED %s -- remove it from the baseline" % key)

    if args.write_baseline:
        with open(args.write_baseline, "w") as f:
            f.write("# generic-spec-matrix open cells (tests/generic-spec-matrix.py)\n")
            for k in sorted(failing):
                f.write(k + "\n")

    print("generic-spec-matrix: %d cells, %d failing (%d new, %d known), %d fixed"
          % (len(results), len(failing), len(new), len(failing) - len(new),
             len(fixed)))
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
