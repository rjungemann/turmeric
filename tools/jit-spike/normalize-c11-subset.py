#!/usr/bin/env python3
"""Replay `tur build`'s __tur_include__ hoist on bare `tur emit-c` output.

Phase J0 scaffolding for docs/upcoming/jit-engine-plan.md -- and, since
findings 16, ONLY the hoist.  This file used to be the c2mir subset
normalizer, carrying three rewrite rules plus a prototype table; every rule
has been retired into the emitter, in the order the findings record:

  rule 3 (`__attribute__((constructor))` -> explicit call)   -- S1b, section 12
  rule 2 (scalar `(T){0}` -> `((T)0)`)                       -- S1, section 11
  rule 1 (`__auto_type` -> named type) + every exact read    -- section 16;
      13,730 sites to zero, verified against the emitted declarations by
      tools/jit-spike/verify-temp-types.py (247k temps, 0 mismatches).

What remains is not a subset fix at all: `tur build` runs an in-process
post-pass that hoists function-scope `__tur_include__` markers to file scope
(src/main.c, hoist_tur_include_directives), and bare `tur emit-c` -- which is
what the spike harness consumes -- does not.  A real `tur jit` runs the same
in-process post-pass and needs no external script; this replay exists only so
the spike can keep feeding emit-c output to c2mir.

    usage: normalize-c11-subset.py <emitted.c> [-o out.c]

(-I is accepted and ignored, so existing harness invocations keep working.)
"""

import argparse
import re
import sys

# A faithful copy of hoist_tur_include_directives() (src/main.c), which
# `tur build` runs over the emitted buffer but bare `tur emit-c` does not.
# Plan section 3.2 step 2 puts this post-pass on the JIT path too, so the
# spike has to do it or every fixture whose inline-C declares a file-scope
# type (httpd's HttpdConn, mbedTLS shims) fails with a bogus "unknown
# typedef".
INCLUDE_MARK_RE = re.compile(r'/\* __tur_include__: (.*?) \*/', re.S)


def hoist_tur_includes(text):
    bodies = INCLUDE_MARK_RE.findall(text)
    if not bodies:
        return text, 0
    # _DEFAULT_SOURCE must precede any hoisted system header -- see the comment
    # on the C original for why (feature-test macros lock in on first include).
    header = '#define _DEFAULT_SOURCE 1\n' + '\n'.join(bodies) + '\n'
    return header + text, len(bodies)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source')
    ap.add_argument('-o', '--out', default='-')
    ap.add_argument('-I', '--include-dir', action='append', default=[],
                    help='accepted for compatibility; unused since findings 16')
    args = ap.parse_args()
    with open(args.source) as f:
        text = f.read()
    result, _ = hoist_tur_includes(text)
    if args.out == '-':
        sys.stdout.write(result)
    else:
        with open(args.out, 'w') as f:
            f.write(result)


if __name__ == '__main__':
    main()
