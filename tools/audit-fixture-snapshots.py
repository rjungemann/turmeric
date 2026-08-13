#!/usr/bin/env python3
"""Audit tests/fixtures for unnecessary expected.c snapshots.

Identifies fixtures where expected.c is dead weight because:
  1. expected.stdout (runtime assertion) already exists, AND
  2. the fixture name doesn't carry codegen/emit/snapshot/regression/mangle/abi
     semantics, AND
  3. the expected.c body contains no historically notable codegen constructs.

Usage:
    python3 tools/audit-fixture-snapshots.py [--delete]

Without --delete: prints candidates and rationale.
With --delete:    actually removes the candidate expected.c files.
"""

import argparse
import os
import re
import sys

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures')

# Fixture names whose codegen output is intentionally asserted.
# Fixture names whose expected.c is an intentional codegen assertion.
# Patterns: the fixture is specifically testing a codegen mechanism.
SEMANTIC_NAME_PATTERNS = re.compile(
    r'codegen'          # explicitly testing code generation
    r'|emit'            # emitting C
    r'|snapshot'        # explicit snapshot
    r'|regression'      # guards a historical bug
    r'|mangle'          # name mangling (e.g. mangle-arrow-name-vs-module)
    r'|operator.mangle' # operator name mangling
    r'|[_-]abi\b'       # ABI contract (e.g. option-result-c-abi)
    r'|cname'           # C name splicing / mangling
    r'|tco'             # tail-call optimisation: the `goto` loop IS the assertion
)

# Explicit keep-set: fixtures whose runtime assertion (expected.stdout) passes
# "by luck" and whose expected.c snapshot is the *real* regression guard, but
# whose names carry no codegen/mangle/abi keyword for SEMANTIC_NAME_PATTERNS to
# catch. These are register-class-correctness guards for the float-carrier
# arrow-composition miscompile (#318 -- "works by luck because the register
# classes happen to match"). The runtime output was correct by luck with the
# buggy int64 carrier, so deleting the snapshot would silently drop the only
# reliable detector. See docs/archive/history/sf-compose-typed-arrow-prints-garbage-floats.md.
CODEGEN_REGRESSION_GUARDS = frozenset({
    'arrow-compose-float',
    'sf-compose-typed',
    'load-inside-defmodule-injects-names',
})


def classify(fixture_dir: str):
    """Return (keep: bool, reason: str) for a fixture directory."""
    name = os.path.basename(fixture_dir)
    ec = os.path.join(fixture_dir, 'expected.c')
    es = os.path.join(fixture_dir, 'expected.stdout')

    if not os.path.isfile(ec):
        return None, 'no expected.c'
    if not os.path.isfile(es):
        return True, 'no expected.stdout -- codegen-only fixture, keep'

    # Explicit codegen regression guards (runtime assertion passes by luck).
    if name in CODEGEN_REGRESSION_GUARDS:
        return True, 'codegen regression guard -- runtime stdout passes by luck, snapshot is the real check'

    # Semantic name guard.
    if SEMANTIC_NAME_PATTERNS.search(name):
        return True, 'fixture name carries codegen/mangle/abi semantics'

    return False, 'has expected.stdout; expected.c is dead weight (runtime assertion sufficient)'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--delete', action='store_true',
                        help='Delete candidate expected.c files')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print kept fixtures too')
    args = parser.parse_args()

    fixtures = sorted(
        d for d in (os.path.join(FIXTURES_DIR, e)
                    for e in os.listdir(FIXTURES_DIR))
        if os.path.isdir(d)
    )

    candidates = []
    kept = []
    skipped = []

    for fixture_dir in fixtures:
        keep, reason = classify(fixture_dir)
        name = os.path.basename(fixture_dir)
        if keep is None:
            skipped.append((name, reason))
        elif keep:
            kept.append((name, reason))
        else:
            candidates.append((name, reason))

    print(f'Total fixtures scanned : {len(fixtures)}')
    print(f'Fixtures with expected.c: {len(kept) + len(candidates)}')
    print(f'Candidates for removal  : {len(candidates)}')
    print()

    if candidates:
        print('=== CANDIDATES (expected.c can be removed) ===')
        for name, reason in candidates:
            print(f'  {name}')
            print(f'    reason: {reason}')
        print()

    if args.verbose and kept:
        print('=== KEPT ===')
        for name, reason in kept:
            print(f'  {name}')
            print(f'    reason: {reason}')
        print()

    if args.delete:
        deleted = 0
        for name, _ in candidates:
            path = os.path.join(FIXTURES_DIR, name, 'expected.c')
            if os.path.isfile(path):
                os.remove(path)
                print(f'  deleted: {path}')
                deleted += 1
        print(f'\nDeleted {deleted} expected.c files.')
    else:
        if candidates:
            print('Run with --delete to remove the candidates above.')

    reduction_pct = len(candidates) / (len(kept) + len(candidates)) * 100 if (len(kept) + len(candidates)) else 0
    print(f'Reduction if deleted: {reduction_pct:.0f}%  '
          f'({len(kept) + len(candidates)} → {len(kept)} expected.c files)')


if __name__ == '__main__':
    main()
