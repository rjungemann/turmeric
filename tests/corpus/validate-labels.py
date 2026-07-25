#!/usr/bin/env python3
"""Validate every corpus benchmark's `:status` label against Z3.

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

Usage:
    pip install z3-solver
    python3 tests/corpus/validate-labels.py [dir]

Exits non-zero if any label disagrees with Z3, or if Z3 itself returns unknown
(a benchmark Z3 cannot settle has no business carrying a definite label).
"""

import sys
import pathlib

try:
    import z3
except ImportError:
    print("validate-labels: z3 not installed; `pip install z3-solver`",
          file=sys.stderr)
    sys.exit(2)

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

        if got == "unknown":
            print(f"  UNKNOWN!  {f}: z3 cannot settle it; drop the label")
            bad += 1
        elif got != want:
            print(f"  MISMATCH! {f}: file says {want}, z3 says {got}")
            bad += 1
        else:
            print(f"  ok        {f} ({want})")

    print(f"\n  checked: {len(files)}   disagreements: {bad}")
    if bad:
        print("validate-labels: FAIL")
        return 1
    print("validate-labels: PASS -- every label agrees with z3")
    return 0


if __name__ == "__main__":
    sys.exit(main())
