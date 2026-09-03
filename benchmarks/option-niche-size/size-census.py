#!/usr/bin/env python3
"""SR3 item 4 -- the size measurement, over the corpus.

The claim under test is "16 -> 8 bytes per value on the eligible population".
Nobody has measured either half of it since the population changed (`String`
became eligible 2026-08-28), so this counts BOTH: how many distinct eligible
`(Option P)` monomorphs the corpus emits, and how many value POSITIONS each
one occupies.

Eligibility is decided by the COMPILER, not re-derived here.  Each input is
emitted twice -- tagged (`TUR_OPTION_NICHE=0`; the niche is the default since
2026-09-03, and `--enable=option-niche` is a no-op) and niche -- and a monomorph is
eligible exactly when its `tur_adt_Option__*` typedef is present in the first
emission and absent from the second.  That is the same discipline SR0(b) used
(the type checker as the oracle rather than grep), and it cannot drift from
`sr3_payload_is_nonnull_pointer` the way a reimplementation would.

What this measures and what it does not:

  MEASURED -- per-value size, exactly, from the emitted struct; and the count
  of declared value positions, which is a static census of where such a value
  can live.

  NOT MEASURED -- bytes saved by a running program.  That needs live-value
  counts, and the graduation measurement already established the shape:
  direct positions win, container elements are at exact parity because both
  representations box at the erased boundary.  A static site count multiplied
  by 8 would be a fabrication, so this script does not print one.
"""
import os, re, subprocess, sys, collections, json, multiprocessing

# The emitter spells a monomorph as a NAMED struct with a nested union of
# per-variant structs, so the body nests two levels deep and a regex over
# balanced braces is the wrong tool.  Find the opening line, then scan.
TYPEDEF_OPEN = re.compile(
    r'typedef\s+struct\s+(?P<name>tur_adt_Option__[A-Za-z0-9_]+)\s*\{')

# A declarator: the type name followed by an identifier, which is a storage
# slot holding a value of that type.  `name *p` is excluded by the negative
# lookahead -- a pointer is a reference to a value living elsewhere, and
# counting it would double-count the thing it points at.
def declarator_re(name):
    return re.compile(r'\b' + re.escape(name) + r'\b\s*(?!\*)[A-Za-z_]')


def emit(tur, path, niche, timeout=60):
    """Emit C for one input.  Returns the source, or None if it did not emit."""
    cmd = [tur, 'emit-c', path]
    # The niche is the default (graduated 2026-09-03); the tagged emission is
    # the TUR_OPTION_NICHE=0 bisection hatch.
    env = dict(os.environ)
    env['TUR_OPTION_NICHE'] = '1' if niche else '0'
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None
    if r.returncode != 0:
        return None
    return r.stdout.decode('utf-8', 'replace')


def typedefs(src):
    """name -> body, by brace-scanning from each typedef's opening line.

    A truncated or unbalanced body is DROPPED rather than guessed at: this
    function's output is the eligibility oracle, and a monomorph silently
    missing from the default emission would read as ineligible, which is the
    one error that would bias the census toward a smaller population.
    """
    out = {}
    for m in TYPEDEF_OPEN.finditer(src):
        i = src.index('{', m.start())
        depth, j = 0, i
        while j < len(src):
            if src[j] == '{':
                depth += 1
            elif src[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        else:
            continue                      # unbalanced -- drop, do not guess
        # The closer must be `} <same name>;` for this to be the typedef of
        # that name rather than some other struct that merely opened here.
        tail = src[j:j + len(m.group('name')) + 8]
        if m.group('name') not in tail:
            continue
        out[m.group('name')] = src[i + 1:j]
    return out


def value_positions(src, name):
    """Count declared storage slots of this monomorph in the emitted C.

    Locals, parameters, returns and struct fields all spell the type by name
    in a declarator, so one pattern covers them.  Excluded: the typedef block
    itself (the declaration OF the type, not a value of it), the `TUR_TY_` /
    `TUR_FN_` include guards (macro keys, not storage), pointer spellings, and
    string literals -- the niche `Some` ctor names the monomorph inside its
    abort message, which is text, not a slot.

    This counts EMISSION sites.  A stdlib body is re-emitted per compilation
    unit, so a corpus-wide SUM of this column would be one body times the
    fixture count -- the trap the CE0 census recorded.  Report it per file.
    """
    rx = declarator_re(name)
    n, in_typedef, depth = 0, False, 0
    for line in src.splitlines():
        s_ = line.strip()
        if not in_typedef and s_.startswith('typedef struct ' + name):
            in_typedef, depth = True, 0
        if in_typedef:
            depth += s_.count('{') - s_.count('}')
            if depth <= 0 and '}' in s_:
                in_typedef = False
            continue
        if s_.startswith('#ifndef ') or s_.startswith('#define '):
            continue
        s_ = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s_)
        n += len(rx.findall(s_))
    return n


def one(path):
    """Census one input.  Returns (path, status, {name: (body, positions)})."""
    tur = os.environ.get('TUR', './build/tur')
    d = emit(tur, path, niche=False)
    if d is None:
        return (path, 'skip-emit-default', {})
    base = typedefs(d)
    if not base:
        return (path, 'no-option-monomorph', {})
    n = emit(tur, path, niche=True)
    if n is None:
        # An input that emits by default and NOT under the flag is itself a
        # finding -- the flag should never break an emission -- so it is
        # reported loudly rather than folded into the skips.
        return (path, 'REGRESSION-emit-niche-failed', {})
    gone = set(base) - set(typedefs(n))
    return (path, 'censused',
            {k: (base[k], value_positions(d, k)) for k in gone},
            len(base) - len(gone))


def main():
    inputs = sys.argv[1:]
    if not inputs:
        print('usage: size-census.py <input.tur>...', file=sys.stderr)
        return 2

    pop = collections.defaultdict(
        lambda: {'body': None, 'positions': 0, 'files': {}})
    stats = collections.Counter()

    jobs = int(os.environ.get('JOBS', multiprocessing.cpu_count()))
    with multiprocessing.Pool(jobs) as pool:
        for rec in pool.imap_unordered(one, inputs, chunksize=8):
            path, status, found = rec[0], rec[1], rec[2]
            stats[status] += 1
            if status == 'REGRESSION-emit-niche-failed':
                print('niche emission failed: %s' % path, file=sys.stderr)
            if len(rec) > 3:
                stats['ineligible-monomorphs-seen'] += rec[3]
            for name, (body, positions) in found.items():
                e = pop[name]
                e['body'] = body
                e['positions'] += positions
                e['files'][path] = positions
            stats['eligible-monomorphs-seen'] += len(found)

    out = {
        'stats': dict(stats),
        'monomorphs': {
            k: {'body': v['body'].strip(),
                'positions_total_emission_sites': v['positions'],
                'n_files': len(v['files']),
                'positions_per_file_max': max(v['files'].values()),
                'positions_per_file_min': min(v['files'].values()),
                'files': sorted(v['files']),
                }
            for k, v in sorted(pop.items(),
                               key=lambda kv: -len(kv[1]['files']))
        },
    }
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
