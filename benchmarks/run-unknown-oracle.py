#!/usr/bin/env python3
"""benchmarks/run-unknown-oracle.py -- which UNKNOWN obligations are actually
valid?  The one number that decides the solver-integer-tail plan's open phases.

The cap sweep counts the SHAPES a phase would decide (`eq no-unit split`,
`LA int feasible` under TUR_REFINE_STATS=1); that is a population, not a
payoff, because most such sets are satisfiable and no complete procedure
could prove them either.  This script asks the payoff question directly: take
every obligation the compiler left `unknown`, hand its dumped VC (`vc_smtlib`
in `--dump-refine=json`) to Z3, and count the ones Z3 calls `unsat` -- those,
and only those, are proofs the in-house chain missed.

DEVELOPMENT SCAFFOLDING, exactly like tests/corpus/validate-labels.py: not
built, not run by the suite, `tur` never links a solver.  Needs
`pip install z3-solver`.

Populations: the fuzzer's own generator (same seeding as the cap sweep) and
every in-tree refinement / GADT fixture.

Usage:
    python3 benchmarks/run-unknown-oracle.py [--n 200] [--seed 1] [--tur ./build/tur]

Reading 2026-09-05: 283 unknown obligations (15 fixture, 268 fuzzer), Z3 sat
283, unsat 0.  Neither Phase 2 (sigma-substitution) nor Phase 3(a)
(branch-and-bound) had a single obligation to win.
"""
import argparse, glob, importlib.util, json, os, random, subprocess, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--tur", default="./build/tur")
    ap.add_argument("--work", default=None)
    args = ap.parse_args()
    try:
        import z3
    except ImportError:
        print("run-unknown-oracle: pip install z3-solver", file=sys.stderr); return 2
    work = args.work or subprocess.run(["mktemp", "-d"], capture_output=True, text=True).stdout.strip()
    spec = importlib.util.spec_from_file_location("rfs", "tests/refine-fuzz-src.py")
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    progs = []
    for i in range(args.n):
        rng = random.Random(args.seed * 1000003 + i)
        p = os.path.join(work, "c%06d.tur" % i)
        open(p, "w").write(m.Gen(rng, "int" if i % 2 == 0 else "float").program())
        progs.append(("fuzz", p))
    for d in sorted(glob.glob("tests/fixtures/*refine*") + glob.glob("tests/fixtures/*gadt*")):
        inp = os.path.join(d, "input.tur")
        if not os.path.exists(inp): inp = os.path.join(d, os.path.basename(d) + ".tur")
        if os.path.exists(inp): progs.append(("fixture", inp))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    tally, missed, unparsed = {}, [], []
    for kind, p in progs:
        try:
            r = subprocess.run([args.tur, "check", "--dump-refine=json", p],
                               capture_output=True, text=True, env=env, timeout=120)
            rep = json.loads(r.stdout)
        except Exception:
            continue
        for ob in rep.get("obligations", []):
            if ob.get("verdict") != "unknown" or not ob.get("vc_smtlib"): continue
            s = z3.Solver(); s.set("timeout", 5000)
            try:
                s.from_string(ob["vc_smtlib"].replace("(get-model)", ""))
                res = str(s.check())
            except Exception as ex:
                res = "unparsed"; unparsed.append((p, str(ex)[:80]))
            tally[(kind, res)] = tally.get((kind, res), 0) + 1
            if res == "unsat": missed.append((kind, p, ob.get("what"), ob.get("predicate")))
    for k in sorted(tally): print("  %-8s %-9s %d" % (k[0], k[1], tally[k]))
    print("unknown obligations Z3 calls UNSAT (missed proofs): %d" % len(missed))
    for kind, p, what, pred in missed: print("  MISSED", kind, p, what, pred)
    for p, e in unparsed: print("  UNPARSED", p, e)
    # A VC the external solver cannot even parse is an unmeasured one, and the
    # serializer's whole promise is external replay -- so that is a failure.
    return 1 if (missed or unparsed) else 0

if __name__ == "__main__":
    sys.exit(main())
