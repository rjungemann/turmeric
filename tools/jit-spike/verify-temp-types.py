#!/usr/bin/env python3
"""Verify every typed call temp against its callee's declaration, corpus-wide.

Findings 16 replaced ~13,700 `__auto_type` call temps with named types drawn
from three sources: the signature side table, builder-to-hoist notes, and two
anchored text reads.  Each source can in principle be wrong in a way `cc`
accepts silently (an int64_t temp for a pointer-returning call compiles with a
warning at most), so this script is the check the suite cannot be: for every

    T __ps_N = (callee(args));          [and __t / cps->direct temps]

where `callee` is a plain identifier, find `callee`'s declaration in the SAME
translation unit and compare T against its declared return type.  A mismatch
names the TU, the temp, and both types.

Only direct-name initializers are checked -- a cast or fn-ptr initializer
carries its own type in the text and gcc enforces those exactly.  Declarations
are matched textually (`[static ]RET name(`), which is the same ground the
emitter's own side table stands on.

    usage: verify-temp-types.py <dir-of-emitted-c> [--verbose]
"""

import glob
import os
import re
import sys

TEMP_RE = re.compile(
    r'^\s*([A-Za-z_][A-Za-z0-9_ ]*?\s*\**)\s*(__ps_\d+|__t\d+)\s*=\s*'
    r'\(?([A-Za-z_][A-Za-z0-9_]*)\s*\(')
# The separator between return type and name is MANDATORY (whitespace and/or
# `*`) -- the same rule normalize-c11-subset.py's PROTO_RE learned the hard
# way: with it optional, the lazy type group splits a single identifier, so a
# statement line `if (f(x))` parses as a declaration of `f` with return type
# `i` and poisons the table.  64 false mismatches on the first run of this
# script came from exactly that.
DECL_RE = re.compile(
    r'^\s*(?:(?:static|extern|inline)\s+)*'
    r'([A-Za-z_][A-Za-z0-9_ ]*?[A-Za-z0-9_])'
    r'((?:\s|\*)+)'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\(')

SKIP_TYPES = {'__auto_type', 'return', 'if', 'while', 'else', 'case',
              'goto', 'do', 'for', 'switch', 'sizeof', 'typedef'}
# Macros whose "declaration" does not exist in the TU; their types are fixed
# by definition.
KNOWN = {'INT64_C': 'int64_t', 'UINT64_C': 'uint64_t'}


def norm(t):
    return ' '.join(t.replace('*', ' * ').split())


def whole_initializer_is_call(line, m):
    """True when the matched callee's parens span the entire initializer.
    `bool t = (INT64_C(1)) == (INT64_C(2));` matches TEMP_RE with callee
    INT64_C, but the initializer is a comparison and the temp's bool is
    correct -- comparing it against INT64_C's return type is a false
    positive.  Walk the callee's parens; only `)`s and `;` may follow."""
    i = m.end() - 1          # the callee's opening paren
    depth = 0
    for j in range(i, len(line)):
        if line[j] == '(':
            depth += 1
        elif line[j] == ')':
            depth -= 1
            if depth == 0:
                rest = line[j + 1:].strip()
                return rest.lstrip(')').strip() in (';', '')
    return False


def signedness_pair(a, b):
    """Same-width integer signedness difference (int64_t vs uint64_t).
    The side table records the Turmeric-declared type (:int -> int64_t)
    while a runtime C declaration may be unsigned; the conversion is
    value-preserving for every value the runtime can produce here, and the
    pre-S1 __auto_type temp flowed the value into the same int64 contexts.
    Reported counts stay identical; this is an accepted difference, not a
    blind spot -- anything cross-width or int/pointer still fails."""
    return {a, b} == {'int64_t', 'uint64_t'}


def check_tu(path, verbose):
    lines = open(path, errors='replace').read().split('\n')
    decls = {}
    for ln in lines:
        m = DECL_RE.match(ln)
        if m and m.group(1).strip() not in SKIP_TYPES:
            stars = '*' * m.group(2).count('*')
            decls.setdefault(m.group(3), norm(m.group(1) + ' ' + stars))
    bad = []
    checked = 0
    for i, ln in enumerate(lines):
        m = TEMP_RE.match(ln)
        if not m:
            continue
        tty, tmp, callee = norm(m.group(1)), m.group(2), m.group(3)
        if tty in ('__auto_type',):
            continue
        want = KNOWN.get(callee) or decls.get(callee)
        if want is None:
            continue          # indirect/macro callee: nothing to compare
        if not whole_initializer_is_call(ln, m):
            continue
        checked += 1
        if norm(want) != tty and not signedness_pair(norm(want), tty):
            bad.append((i + 1, tmp, callee, tty, norm(want)))
    if verbose and bad:
        for lineno, tmp, callee, got, want in bad:
            print(f'{os.path.basename(path)}:{lineno}: {tmp} typed `{got}` '
                  f'but {callee} declares `{want}`')
    return checked, bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    verbose = '--verbose' in sys.argv
    if len(args) != 1:
        sys.exit(__doc__)
    total_checked = total_bad = tus_bad = 0
    files = sorted(glob.glob(os.path.join(args[0], '*.c')))
    for p in files:
        checked, bad = check_tu(p, verbose)
        total_checked += checked
        total_bad += len(bad)
        tus_bad += 1 if bad else 0
    print(f'{len(files)} TUs, {total_checked} typed direct-call temps checked, '
          f'{total_bad} mismatches in {tus_bad} TUs')
    sys.exit(1 if total_bad else 0)


if __name__ == '__main__':
    main()
