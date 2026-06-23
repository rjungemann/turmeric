#!/usr/bin/env python3
"""check_turi_parity.py -- TI8 CI ratchet for tur <-> turi expression parity.

The tree-walking interpreter (src/turi/eval.c) should be able to evaluate every
EX_* expression kind the compiler can emit (src/compiler/expr.h), modulo a small
set of documented carve-outs listed in docs/artifacts/turi-carve-out.txt.

This script enforces that invariant so the parity gap cannot silently grow:

  1. Collect every EX_* enumerator from the ExprKind enum in expr.h.
  2. Collect every `case EX_*:` arm in eval.c.
  3. Any kind in (1) but not (2) must appear in the carve-out file, with a
     one-line rationale.  An unhandled, un-carved-out kind fails the build.
  4. A carve-out entry for a kind that *is* handled, or that names a
     nonexistent kind, is a stale entry and also fails (keeps the list honest).

Exit status: 0 on parity, 1 on any violation.

Usage:
  python3 tools/check_turi_parity.py
  python3 tools/check_turi_parity.py --quiet     # only print on failure
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPR_H = os.path.join(ROOT, "src", "compiler", "expr.h")
EVAL_C = os.path.join(ROOT, "src", "turi", "eval.c")
CARVE_OUT = os.path.join(ROOT, "docs", "artifacts", "turi-carve-out.txt")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def enum_kinds():
    """Every EX_* enumerator inside the ExprKind enum block of expr.h."""
    text = read(EXPR_H)
    m = re.search(r"typedef enum ExprKind\s*\{(.*?)\}\s*ExprKind;", text, re.S)
    if not m:
        sys.exit("check_turi_parity: could not find ExprKind enum in expr.h")
    body = m.group(1)
    # Strip /* ... */ comments so a commented EX_ token never counts.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return set(re.findall(r"\b(EX_[A-Z0-9_]+)\b", body))


def handled_kinds():
    """Every EX_* named in a `case EX_*:` arm of eval.c."""
    text = read(EVAL_C)
    return set(re.findall(r"\bcase\s+(EX_[A-Z0-9_]+)\s*:", text))


def carve_outs():
    """Map of carved-out EX_* -> rationale from docs/artifacts/turi-carve-out.txt.

    Format: one `EX_NAME -- rationale` per line; blank lines and `#` comments
    are ignored.
    """
    result = {}
    if not os.path.exists(CARVE_OUT):
        return result
    for raw in read(CARVE_OUT).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(EX_[A-Z0-9_]+)\s*--\s*(.+)$", line)
        if not m:
            sys.exit(f"check_turi_parity: malformed carve-out line: {raw!r}\n"
                     f"  expected: EX_NAME -- one-line rationale")
        result[m.group(1)] = m.group(2).strip()
    return result


def main():
    quiet = "--quiet" in sys.argv[1:]
    kinds = enum_kinds()
    handled = handled_kinds() & kinds
    carved = carve_outs()

    unhandled = kinds - handled
    # Kinds that are unhandled and not carved out: hard failure.
    uncovered = sorted(unhandled - set(carved))
    # Carve-out entries that are actually handled or do not exist: stale.
    stale_handled = sorted(set(carved) & handled)
    stale_unknown = sorted(set(carved) - kinds)

    ok = not (uncovered or stale_handled or stale_unknown)

    if ok and not quiet:
        print(f"turi parity OK: {len(handled)}/{len(kinds)} EX_* kinds handled, "
              f"{len(carved)} carved out, 0 gaps.")
    if not ok:
        print("turi parity CHECK FAILED", file=sys.stderr)
        if uncovered:
            print("\nUnhandled EX_* kinds with no carve-out entry "
                  "(add a `case` arm in src/turi/eval.c, or document the\n"
                  "carve-out in docs/artifacts/turi-carve-out.txt):", file=sys.stderr)
            for k in uncovered:
                print(f"  - {k}", file=sys.stderr)
        if stale_handled:
            print("\nStale carve-out entries (these kinds ARE handled now -- "
                  "remove them from docs/artifacts/turi-carve-out.txt):", file=sys.stderr)
            for k in stale_handled:
                print(f"  - {k}", file=sys.stderr)
        if stale_unknown:
            print("\nUnknown carve-out entries (no such EX_* enumerator -- "
                  "fix the name in docs/artifacts/turi-carve-out.txt):", file=sys.stderr)
            for k in stale_unknown:
                print(f"  - {k}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
