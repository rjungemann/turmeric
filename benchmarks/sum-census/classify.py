#!/usr/bin/env python3
"""Join the SR0(a) census against each constructor's declaring type.

A BOXED ctor is not automatically SR1's customer.  A `:heap` ADT is a typed
pointer BY DESIGN -- boxing it is correct, and no by-value lowering applies.
What SR1 can actually fix is a NON-heap, multi-variant, non-recursive sum; the
recursive ones are SR4's, and need field-level boxing rather than by-value.
"""
import re, os, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def scan():
    """ctor-name -> (type, kind) where kind is one of:
       product      single-variant (defstruct or 1-ctor defdata) -- already by value
       heap         declared :heap -- a typed pointer by design
       gadt         defgadt -- keeps the tagged union by design (adt_is_flat_product
                    excludes is_gadt), so no by-value lowering applies
       sum-flat     multi-variant, non-recursive        -> SR1
       sum-rec      multi-variant, self-recursive       -> SR4
    """
    m = {}
    files = []
    for d in ('stdlib', 'tests/fixtures', 'examples', 'benchmarks'):
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            files += [os.path.join(dp, f) for f in fs if f.endswith('.tur')]
    for f in files:
        try: src = open(f, encoding='utf-8', errors='replace').read()
        except OSError: continue
        for mo in re.finditer(r'\((defdata|defstruct|defgadt)\s+([A-Za-z0-9_?!*<>=+-]+)', src):
            form, name = mo.group(1), mo.group(2)
            i, depth = mo.start(), 0
            for j in range(i, min(len(src), i + 8000)):
                if src[j] == '(': depth += 1
                elif src[j] == ')':
                    depth -= 1
                    if depth == 0: break
            body = src[i:j + 1]
            head = body[mo.end() - i:]
            is_heap = bool(re.match(r'\s*:heap\b', head))
            if form == 'defstruct':
                m.setdefault(name, (name, 'heap' if is_heap else 'product'))
                continue
            if form == 'defgadt':
                for c in re.findall(r'\(([A-Za-z0-9_?!*<>=+-]+)', body[len(form) + len(name) + 2:]):
                    if c not in (':copy', ':heap'):
                        m.setdefault(c, (name, 'gadt'))
                continue
            ctors = re.findall(r'\(([A-Za-z0-9_?!*<>=+-]+)', body[len(form) + len(name) + 2:])
            ctors = [c for c in ctors if c not in (':copy', ':heap')]
            nvar = len(ctors)
            rec = bool(re.search(r'[\s(:]' + re.escape(name) + r'[\s)]', body[len(name) + 9:]))
            if is_heap:      kind = 'heap'
            elif nvar <= 1:  kind = 'product'
            elif rec:        kind = 'sum-rec'
            else:            kind = 'sum-flat'
            for c in ctors:
                m.setdefault(c, (name, kind))
    return m

def main(path):
    meta = scan()
    calls, fixtures, unknown = collections.Counter(), collections.defaultdict(set), collections.Counter()
    for line in open(path, encoding='utf-8'):
        parts = line.rstrip('\n').split('\t')
        if len(parts) != 4: continue
        fix, ctor, n, rep = parts[0], parts[1], int(parts[2]), parts[3]
        base = ctor[len('ctor_'):]
        base = base.split('__')[0]                      # strip the monomorph suffix
        if base in meta:
            kind = meta[base][1]
        else:
            kind = 'unknown'; unknown[base] += n
        calls[kind] += n
        fixtures[kind].add(fix)

    total = sum(calls.values())
    allfix = len(set().union(*fixtures.values())) if fixtures else 0
    print("class      constructions   share    fixtures  (of %d that construct anything)" % allfix)
    for k in ('product', 'heap', 'gadt', 'sum-flat', 'sum-rec', 'unknown'):
        if k in calls:
            print("  %-9s %12d  %5.1f%%   %6d" %
                  (k, calls[k], 100.0 * calls[k] / total, len(fixtures[k])))
    if unknown:
        print("\n  unresolved ctors (top 5):", unknown.most_common(5))

if __name__ == '__main__':
    main(sys.argv[1])
