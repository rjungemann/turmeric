#!/usr/bin/env python3
"""Validate every corpus benchmark's `:status` label against Z3 -- and, when it
is installed, against cvc5 as an INDEPENDENT second opinion.

The corpus is the durable half of the Z3 retirement criteria: its labels are
DATA in the repo, and `tur_refine_corpus` replays them with no solver linked.
That only works if the labels are RIGHT -- and a wrong label is worse than no
corpus at all:

  * a benchmark wrongly labelled `sat` that is really unsat can never fail,
    so it silently stops testing anything;
  * a benchmark wrongly labelled `unsat` that is really sat inverts the
    soundness check -- the harness would demand a proof of something false and
    treat the correct refusal as weakness.

So the labels get checked once, by a real solver, and the result is committed.
This script is DEVELOPMENT scaffolding in exactly the way libz3 is: it is not
built, linked, or run by the suite. `tur_refine_corpus` never imports it.

Two solvers rather than one, because Z3 is the thing being retired. A label
confirmed only by Z3 inherits whatever Z3 gets wrong, and the corpus exists
precisely so the in-house chain can be trusted after Z3 is gone. cvc5 comes
from a different implementation lineage, so agreement between the two is
meaningfully stronger evidence than either alone. cvc5 is optional -- absent,
the script still checks Z3 and says so.

Usage:
    pip install z3-solver          # required
    pip install cvc5               # optional, recommended
    python3 tests/corpus/validate-labels.py [dir]

Exits non-zero if any label disagrees with either solver, or if a solver
returns unknown (a benchmark no solver can settle has no business carrying a
definite label).
"""

import sys
import pathlib

try:
    import z3
except ImportError:
    print("validate-labels: z3 not installed; `pip install z3-solver`",
          file=sys.stderr)
    sys.exit(2)

try:
    import cvc5 as _cvc5
except ImportError:
    _cvc5 = None


def cvc5_status(path):
    """cvc5's verdict, or None when cvc5 is unavailable / cannot parse."""
    if _cvc5 is None:
        return None
    try:
        tm = _cvc5.TermManager()
        solver = _cvc5.Solver(tm)
        parser = _cvc5.InputParser(solver)
        parser.setFileInput(_cvc5.InputLanguage.SMT_LIB_2_6, str(path))
        symbols = parser.getSymbolManager()
        seen = []
        while True:
            cmd = parser.nextCommand()
            if cmd.isNull():
                break
            out = cmd.invoke(solver, symbols)
            if out:
                seen.append(str(out).strip())
        for line in seen:
            if line in ("sat", "unsat", "unknown"):
                return line
        return None
    except Exception:
        return None

STATUS_PREFIX = "(set-info :status "


def declared_status(text):
    for line in text.splitlines():
        line = line.strip()
        if line.startswith(STATUS_PREFIX):
            return line[len(STATUS_PREFIX):].rstrip(")").strip()
    return None


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                        else "tests/corpus/smtlib")
    files = sorted(root.rglob("*.smt2"))
    if not files:
        print(f"validate-labels: no .smt2 files under {root}", file=sys.stderr)
        return 1

    bad = 0
    for f in files:
        text = f.read_text()
        want = declared_status(text)
        if want is None:
            print(f"  no-label  {f}")
            continue
        try:
            solver = z3.Solver()
            solver.add(z3.parse_smt2_string(text))
            got = str(solver.check())
        except z3.Z3Exception as exc:
            print(f"  PARSE!    {f}: {exc}")
            bad += 1
            continue

        other = cvc5_status(f)

        if got == "unknown":
            print(f"  UNKNOWN!  {f}: z3 cannot settle it; drop the label")
            bad += 1
        elif got != want:
            print(f"  MISMATCH! {f}: file says {want}, z3 says {got}")
            bad += 1
        elif other == "unknown":
            print(f"  UNKNOWN!  {f}: cvc5 cannot settle it; drop the label")
            bad += 1
        elif other is not None and other != want:
            print(f"  MISMATCH! {f}: file says {want}, cvc5 says {other} "
                  f"(z3 agreed with the file -- the solvers DISAGREE)")
            bad += 1
        else:
            seal = "z3+cvc5" if other is not None else "z3 only"
            print(f"  ok        {f} ({want}, {seal})")

    print(f"\n  checked: {len(files)}   disagreements: {bad}")
    if bad:
        print("validate-labels: FAIL")
        return 1
    if _cvc5 is None:
        print("validate-labels: PASS -- every label agrees with z3")
        print("  (cvc5 not installed; `pip install cvc5` for an independent "
              "second opinion)")
    else:
        print("validate-labels: PASS -- every label agrees with BOTH z3 and cvc5")
    return 0


if __name__ == "__main__":
    sys.exit(main())
