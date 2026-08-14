#!/usr/bin/env python3
"""check_crossing_routing.py -- R4 ratchet for carrier<->concrete crossing routing.

docs/archive/history/carrier-crossing-recovery-routing-plan.md turns the carrier<->
concrete recovery routines into mandatory chokepoints.  R3 enforces their
*runtime* postcondition (a non-concrete recovered type is a Debug ICE); this
script is R4's *static* enforcement: the audit registry is the single source of
truth for which crossings are routed, and "a crossing without a row fails
review" becomes "a crossing without a row fails the suite."

It works like check_turi_parity.py:

  1. Enumerate every call site of the sanctioned recovery chokepoints in
     src/compiler/*.c (definitions and comments excluded).
  2. Compare the (routine, file, count) triples against the audit registry
     docs/artifacts/crossing-routing-audit.txt.
  3. Any new call site (a crossing that forgot its row), a changed count, or a
     stale registry entry fails the check.

Adding or removing a chokepoint call site is therefore a deliberate act: rerun
with --update to regenerate the registry, and add the matching row to the audit
tables in the plan doc.

Exit status: 0 when the registry matches the source, 1 on any drift.

Usage:
  python3 tools/check_crossing_routing.py
  python3 tools/check_crossing_routing.py --quiet    # only print on failure
  python3 tools/check_crossing_routing.py --update    # rewrite the registry
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPILER_DIR = os.path.join(ROOT, "src", "compiler")
REGISTRY = os.path.join(ROOT, "docs", "artifacts", "crossing-routing-audit.txt")

# The sanctioned carrier<->concrete recovery chokepoints.  Every call site of
# these is a routed crossing and must be accounted for in the registry.
CHOKEPOINTS = [
    # value side
    "emit_spec_arg_type_for_binding",
    "emit_var_spec_arg_type",
    # dispatch side
    "emit_reresolve_disp_type",
    "emit_dispatch_tyvar",
    "emit_abi_constraint_var_bindings",
    # fn-value side
    "emit_abi_fn_value_signature",
    # R3 gate (the runtime enforcement these call sites lean on)
    "emit_abi_assert_routed_concrete",
]


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# Control-flow keywords that can sit at line start directly before a call name
# (`return NAME(`, `else NAME(`); they look like a type token to the definition
# regex but introduce a call, not a definition.
_NOT_TYPES = {"return", "if", "else", "while", "for", "switch", "do", "sizeof"}


def is_definition(line, name):
    """A definition line is `[static] [const] <type> [*] NAME(` -- the routine
    name immediately follows a type token.  A call never has that shape (the
    name follows `=`, `(`, `!`, `return`, `&&`, `.`/`->`, etc.)."""
    m = re.match(
        r"^\s*(static\s+)?(const\s+)?([A-Za-z_]\w*)\s*\**\s*" + name + r"\s*\(",
        line,
    )
    if not m:
        return False
    # The token immediately before NAME must be a type, not a control keyword.
    return m.group(3) not in _NOT_TYPES


def is_comment(line):
    s = line.lstrip()
    return s.startswith("*") or s.startswith("//") or s.startswith("/*")


def call_sites():
    """Map (routine, file) -> count of call sites across src/compiler/*.c."""
    counts = {}
    for fn in sorted(os.listdir(COMPILER_DIR)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(COMPILER_DIR, fn)
        for line in read(path).splitlines():
            if is_comment(line):
                continue
            for name in CHOKEPOINTS:
                if not re.search(r"\b" + name + r"\s*\(", line):
                    continue
                if is_definition(line, name):
                    continue
                counts[(name, fn)] = counts.get((name, fn), 0) + 1
    return counts


def fmt(counts):
    """Deterministic registry text: `routine file count`, sorted."""
    lines = [f"{name} {fn} {n}" for (name, fn), n in counts.items()]
    return "\n".join(sorted(lines)) + "\n"


def parse_registry():
    counts = {}
    if not os.path.exists(REGISTRY):
        return counts
    for raw in read(REGISTRY).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 3 or not parts[2].isdigit():
            sys.exit(f"check_crossing_routing: malformed registry line: {raw!r}\n"
                     f"  expected: <routine> <file.c> <count>")
        counts[(parts[0], parts[1])] = int(parts[2])
    return counts


HEADER = (
    "# crossing-routing-audit.txt -- R4 registry of carrier<->concrete recovery\n"
    "# chokepoint call sites (docs/archive/carrier-crossing-recovery-routing-\n"
    "# plan.md).  Generated/checked by tools/check_crossing_routing.py.\n"
    "#\n"
    "# Format: <routine> <file.c> <call-site-count>.  Regenerate with\n"
    "#   python3 tools/check_crossing_routing.py --update\n"
    "# and add a matching row to the audit tables in the plan doc.\n"
)


def main():
    args = sys.argv[1:]
    quiet = "--quiet" in args
    update = "--update" in args
    actual = call_sites()

    if update:
        with open(REGISTRY, "w", encoding="utf-8") as f:
            f.write(HEADER)
            f.write(fmt(actual))
        print(f"crossing-routing registry updated: {len(actual)} call sites.")
        return 0

    expected = parse_registry()
    added = sorted(set(actual) - set(expected))
    removed = sorted(set(expected) - set(actual))
    changed = sorted(k for k in set(actual) & set(expected)
                     if actual[k] != expected[k])

    ok = not (added or removed or changed)
    if ok and not quiet:
        print(f"crossing-routing OK: {len(actual)} chokepoint call sites, "
              f"all accounted for in the audit registry.")
    if not ok:
        print("crossing-routing CHECK FAILED", file=sys.stderr)
        if added:
            print("\nNew chokepoint call site(s) with no audit row -- a carrier<->"
                  "concrete crossing\nwas added without registering it.  Run "
                  "`python3 tools/check_crossing_routing.py --update`\nand add a "
                  "row to the audit tables in the plan doc:", file=sys.stderr)
            for name, fn in added:
                print(f"  + {name} {fn} ({actual[(name, fn)]})", file=sys.stderr)
        if changed:
            print("\nChanged call-site count(s) (a crossing was added/removed in "
                  "an already-routed file):", file=sys.stderr)
            for name, fn in changed:
                print(f"  ~ {name} {fn}: {expected[(name, fn)]} -> "
                      f"{actual[(name, fn)]}", file=sys.stderr)
        if removed:
            print("\nStale registry entry(ies) (no such call site any more -- "
                  "regenerate the registry):", file=sys.stderr)
            for name, fn in removed:
                print(f"  - {name} {fn} ({expected[(name, fn)]})", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
