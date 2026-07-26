#!/usr/bin/env python3
"""Generate labelled SMT-LIB2 benchmarks in the fragment the in-house chain
decides, with `:status` supplied by Z3.

This is the other half of using Z3 as SCAFFOLDING rather than as a dependency:
it runs once, at authoring time, on a machine that happens to have Z3, and what
it leaves behind is a corpus of labelled files. `tur_refine_corpus` replays
those with no solver linked, which is what has to keep working after
refine_libz3.c is deleted.

Two uses:

  * a SOAK -- generate thousands into a scratch directory, replay them, and
    look for the chain proving a satisfiable benchmark contradictory;
  * a CURATION -- keep a bounded, committed subset so the standing regression
    covers the fragment without the repo carrying a benchmark farm.

`sat` benchmarks are the soundness-relevant ones (a wrong VALID there breaks
the one-directional invariant), so generation is biased toward them and the
curator keeps the split even.

Usage:
    pip install z3-solver
    python3 tests/corpus/generate-corpus.py --out DIR --n 2000 [--seed 1]
"""

import argparse
import pathlib
import random
import sys

try:
    import z3
except ImportError:
    print("generate-corpus: z3 not installed; `pip install z3-solver`",
          file=sys.stderr)
    sys.exit(2)

INT_VARS = ["x", "y", "z", "w"]
REAL_VARS = ["p", "q", "r"]


class Gen:
    def __init__(self, rng, mode):
        self.rng = rng
        self.mode = mode          # "int" | "real" | "uf" | "mixed"
        self.uses_uf = False

    # -- terms ------------------------------------------------------------
    def var(self):
        return self.rng.choice(REAL_VARS if self.mode == "real" else INT_VARS)

    def lit(self):
        if self.mode == "real":
            return "%.1f" % self.rng.choice([-2.5, -1.5, 0.0, 0.5, 1.5, 2.5, 3.5])
        return str(self.rng.choice([-3, -2, -1, 0, 1, 2, 3, 5, 10]))

    def atom_term(self, depth):
        r = self.rng.random()
        if depth <= 0 or r < 0.45:
            return self.var() if r < 0.30 else self.lit()
        if self.mode in ("uf", "mixed") and r < 0.62:
            self.uses_uf = True
            if self.rng.random() < 0.5:
                return "(f %s)" % self.atom_term(depth - 1)
            return "(g %s %s)" % (self.atom_term(depth - 1),
                                  self.atom_term(depth - 1))
        op = self.rng.choice(["+", "-", "*"])
        if op == "*":
            # keep it linear: scale by a literal, which is what the arithmetic
            # stage actually decides
            return "(* %s %s)" % (self.lit(), self.var())
        return "(%s %s %s)" % (op, self.atom_term(depth - 1),
                               self.atom_term(depth - 1))

    def rel(self, depth):
        op = self.rng.choice(["<", "<=", ">", ">=", "=", "="])
        return "(%s %s %s)" % (op, self.atom_term(depth), self.atom_term(depth))

    def formula(self, depth):
        r = self.rng.random()
        if depth <= 0 or r < 0.5:
            return self.rel(2)
        if r < 0.62:
            return "(not %s)" % self.formula(depth - 1)
        if r < 0.76:
            return "(and %s %s)" % (self.formula(depth - 1), self.formula(depth - 1))
        if r < 0.88:
            return "(or %s %s)" % (self.formula(depth - 1), self.formula(depth - 1))
        return "(=> %s %s)" % (self.formula(depth - 1), self.formula(depth - 1))

    # -- whole benchmark --------------------------------------------------
    def benchmark(self):
        n_asserts = self.rng.randint(2, 5)
        body = [self.formula(2) for _ in range(n_asserts)]
        decls = []
        names = REAL_VARS if self.mode == "real" else INT_VARS
        sort = "Real" if self.mode == "real" else "Int"
        for v in names:
            decls.append("(declare-fun %s () %s)" % (v, sort))
        if self.mode in ("uf", "mixed"):
            decls.append("(declare-fun f (%s) %s)" % (sort, sort))
            decls.append("(declare-fun g (%s %s) %s)" % (sort, sort, sort))
        logic = {"int": "QF_LIA", "real": "QF_LRA",
                 "uf": "QF_UFLIA", "mixed": "QF_UFLIA"}[self.mode]
        return logic, decls, body


def render(logic, decls, body, status):
    lines = ["(set-logic %s)" % logic, "(set-info :status %s)" % status]
    lines += decls
    lines += ["(assert %s)" % b for b in body]
    lines += ["(check-sat)", "(exit)"]
    return "\n".join(lines) + "\n"


def label(text):
    try:
        s = z3.Solver()
        s.set("timeout", 5000)
        s.add(z3.parse_smt2_string(text))
        return str(s.check())
    except z3.Z3Exception:
        return "parse-error"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    kept = {"sat": 0, "unsat": 0}
    dropped = 0
    for i in range(args.n):
        mode = rng.choice(["int", "int", "real", "uf", "mixed"])
        g = Gen(rng, mode)
        logic, decls, body = g.benchmark()
        probe = render(logic, decls, body, "unknown")
        st = label(probe)
        if st not in ("sat", "unsat"):
            dropped += 1
            continue
        name = "gen_%s_%s_%05d.smt2" % (mode, st, i)
        (out / name).write_text(render(logic, decls, body, st))
        kept[st] += 1

    print("generated into %s" % out)
    print("  sat   : %d" % kept["sat"])
    print("  unsat : %d" % kept["unsat"])
    print("  dropped (z3 unknown / parse) : %d" % dropped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
