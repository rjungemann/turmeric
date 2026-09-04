#!/usr/bin/env python3
"""Count CONSTRUCTION SITES per representation class in a tree of .tur source.

The dynamic census (run-census.sh) measures what executes, which needs a
runnable workload.  A library ecosystem has no canonical workload, and its
spice tests need vendored C headers that only `tur build` resolves -- so for
libraries, count sites in the source instead.  Counting them in EMITTED C would
not work: every file inlines the whole autoloaded stdlib, so stdlib ctor
definitions appear everywhere and user call sites cannot be separated.

Classes match classify.py: product / heap / gadt / sum-flat / sum-rec, plus
optres for the Option/Result constructor functions, which are `defstruct`
records today and so would otherwise land in `product`.

LIMITATION -- read before quoting any number this prints.  Constructor names
are NOT unique across a multi-repo corpus: stdlib's `Cons` is a `:heap`
defstruct, while several fixtures and spices declare their own `Cons` variant
of a local sum.  The name->class map takes whichever declaration is scanned
first, so per-class totals are unreliable wherever a name collides.  The one
row that is safe is `optres`, which keys on the fixed function names
some/none/ok/err and cannot collide.  For everything else prefer the DYNAMIC
census (run-census.sh), which reads real emitted ctor symbols, or the
declaration-form counts, which are a plain grep over decl keywords.
"""
import re, os, sys, collections

DECL = re.compile(r'\((defdata|defstruct|defgadt)\s+([A-Za-z0-9_?!*<>=+-]+)')

def scan_decls(roots):
    m = {}
    for root in roots:
        for dp, _, fs in os.walk(root):
            for fn in fs:
                if not fn.endswith('.tur'):
                    continue
                p = os.path.join(dp, fn)
                try: raw = open(p, encoding='utf-8', errors='replace').read()
                except OSError: continue
                # Depth counting must not see parens inside comments or strings:
                # one stray '(' in a ;; line ran `body` to the 8000-char cap and
                # swept in every following token as a constructor, which is how
                # `int`, `fn` and `a` ended up classified as sum constructors.
                src = re.sub(r'"(?:[^"\\]|\\.)*"', '""', raw)
                src = re.sub(r';[^\n]*', '', src)
                for mo in DECL.finditer(src):
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
                        if name[:1].isupper():
                            m.setdefault(name, 'heap' if is_heap else 'product')
                        continue
                    ctors = [c for c in re.findall(r'\(([A-Za-z0-9_?!*<>=+-]+)',
                                                   body[len(form) + len(name) + 2:])
                             if c not in (':copy', ':heap')]
                    if form == 'defgadt':      kind = 'gadt'
                    elif is_heap:              kind = 'heap'
                    elif len(ctors) <= 1:      kind = 'product'
                    elif re.search(r'[\s(:]' + re.escape(name) + r'[\s)]', body[len(name) + 9:]):
                        kind = 'sum-rec'
                    else:                      kind = 'sum-flat'
                    for c in ctors:
                        if c[:1].isupper():        # ctor names are capitalised
                            m.setdefault(c, kind)
    return m

def count(root, meta):
    hits, per_ctor = collections.Counter(), collections.Counter()
    for dp, _, fs in os.walk(root):
        for fn in fs:
            if not fn.endswith('.tur'):
                continue
            try: src = open(os.path.join(dp, fn), encoding='utf-8', errors='replace').read()
            except OSError: continue
            src = re.sub(r';[^\n]*', '', src)                 # strip comments
            for mo in re.finditer(r'\(([A-Za-z0-9_?!*<>=+-]+)', src):
                nm = mo.group(1)
                if nm in ('some', 'none', 'ok', 'err'):
                    hits['optres'] += 1; per_ctor[nm] += 1
                elif nm in meta and meta[nm] not in ('product',) :
                    hits[meta[nm]] += 1; per_ctor[nm] += 1
                elif nm in meta:
                    hits['product'] += 1
    return hits, per_ctor

if __name__ == '__main__':
    roots = sys.argv[1:]
    meta = scan_decls(roots)
    for r in roots:
        h, pc = count(r, meta)
        tot = sum(h.values()) or 1
        print("\n%s" % r)
        for k in ('optres', 'product', 'heap', 'gadt', 'sum-flat', 'sum-rec'):
            if h[k]:
                print("  %-9s %6d sites  %5.1f%%" % (k, h[k], 100.0 * h[k] / tot))
        print("  top:", ', '.join('%s=%d' % kv for kv in pc.most_common(6)))
