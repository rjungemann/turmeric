#!/usr/bin/env python3
"""Inject per-constructor call counters into emitted Turmeric C.

SR0(a) (docs/upcoming/sum-representation-plan.md) needs to know how often small
sums are actually CONSTRUCTED at runtime, not how many construction sites exist
in the source.  This instruments the emitted C rather than adding a codegen
flag, for two reasons: the measurement must not perturb the codegen it is
measuring (the ADT slab work broke every fixture snapshot by emitting into the
preamble unconditionally), and the classification it feeds already lives
outside the compiler.

Each `ctor_*` definition gets a counter bump as its first statement.  A
destructor dumps `name<TAB>count<TAB>repr` at exit.  `repr` is read off the
emitted signature: a ctor returning `int64_t` and calling malloc is BOXED, one
returning an aggregate is BYVAL -- which is exactly the SR1/SR2 distinction.
"""
import re, sys

CTOR = re.compile(r'^static\s+(.+?)\s+(ctor_[A-Za-z0-9_]+)\s*\(([^;]*)\)\s*\{\s*$')

def main(src_path, out_path):
    lines = open(src_path, encoding='utf-8', errors='replace').read().split('\n')
    ctors = []          # (name, repr)
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = CTOR.match(line)
        if m:
            name = m.group(2)
            # Look ahead over the body for a malloc: BOXED vs BYVAL.
            body, depth, j = [], 0, i
            while j < len(lines):
                depth += lines[j].count('{') - lines[j].count('}')
                body.append(lines[j])
                j += 1
                if depth <= 0:
                    break
            rep = 'BOXED' if any('malloc' in b or 'slab_alloc' in b for b in body) else 'BYVAL'
            idx = len(ctors)
            ctors.append((name, rep))
            out.append(line)
            out.append('    __tur_census[%d]++;' % idx)
            i += 1
            continue
        out.append(line)
        i += 1

    if not ctors:
        open(out_path, 'w', encoding='utf-8').write('\n'.join(out))
        return 0

    names = ', '.join('"%s"' % n for n, _ in ctors)
    reps  = ', '.join('"%s"' % r for _, r in ctors)
    preamble = '''
/* ---- SR0(a) construction census (injected, not emitted) ---- */
#include <stdio.h>
#include <stdlib.h>
static unsigned long __tur_census[%d];
static const char *__tur_census_names[] = { %s };
static const char *__tur_census_reprs[] = { %s };
static void __tur_census_dump(void) __attribute__((destructor));
static void __tur_census_dump(void) {
    const char *p = getenv("TUR_CENSUS_OUT");
    FILE *f = p ? fopen(p, "a") : stderr;
    if (!f) return;
    for (int i = 0; i < %d; i++)
        if (__tur_census[i])
            fprintf(f, "%%s\\t%%lu\\t%%s\\n", __tur_census_names[i],
                    __tur_census[i], __tur_census_reprs[i]);
    if (p) fclose(f);
}
/* ---- end census ---- */
''' % (len(ctors), names, reps, len(ctors))

    # Insert at the last PREPROCESSOR-DEPTH-ZERO line before the first ctor.
    # Anchoring on "after the last #include" put the array inside the emitted
    # `#ifdef _WIN32` winsock block, so on Linux it vanished and every injected
    # bump was an undeclared identifier.  Conditional compilation makes line
    # order a bad proxy for scope; track the nesting instead.
    first_ctor = next(n for n, l in enumerate(out) if CTOR.match(l))
    depth, anchor = 0, 0
    for n, l in enumerate(out[:first_ctor]):
        t = l.lstrip()
        if re.match(r'#\s*(if|ifdef|ifndef)\b', t):
            depth += 1
        elif re.match(r'#\s*endif\b', t):
            depth = max(0, depth - 1)
        elif depth == 0:
            anchor = n
    out.insert(anchor + 1, preamble)
    open(out_path, 'w', encoding='utf-8').write('\n'.join(out))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
