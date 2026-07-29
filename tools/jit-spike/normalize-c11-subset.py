#!/usr/bin/env python3
"""Rewrite `tur emit-c` output into c2mir's accepted C11 subset.

Phase J0 scaffolding for docs/upcoming/jit-engine-plan.md.  This script is NOT
a design element of `tur jit` -- it exists so the J0 spike can run real
fixtures today, and every rule in it is an item of S1 work (plan section 4)
that belongs in the emitter instead.  When S1 lands, this file is deleted.

It also replays one post-pass that `tur build` runs but bare `tur emit-c` does
not (the `__tur_include__` hoist); that one is not a subset fix, it is step 2
of the plan's execution flow.

Three subset rewrites.  The first two are driven by hard c2mir parse errors;
the third repairs something c2mir accepts and then silently discards, which is
the more dangerous kind.

  1. `__auto_type x = (E);`  ->  `T x = (E);`
     GNU C only; c2mir registers `typeof` as a keyword but never wires it into
     the grammar, so there is no in-language spelling to macro it onto.  `T` is
     recovered exactly, not guessed: `E` is always a call, and a call's type is
     its callee's declared return type, which is in the same translation unit.

  2. `(T){0}`  ->  `((T)0)`  for scalar T
     A scalar compound literal is C99-legal; c2mir rejects it outright
     ("braces around scalar initializer").  Aggregate `(T){0}` is left alone.

  3. `__attribute__((constructor))` -> an explicit call sequence at the top of
     main.  c2mir drops GCC attributes without a diagnostic; dropping this one
     costs a SIGSEGV in effectful code and wrong answers in dynamic variables.

`__attribute__((cleanup(f)))` has NO rewrite here and none is possible: a
scope-exit destructor cannot be recovered from outside the compiler.  It is the
one known correctness gap the spike leaves open -- see the findings doc.

Anything this script cannot resolve is reported on stderr and left verbatim,
so a c2mir failure downstream names a real gap rather than a silent miss.

    usage: normalize-c11-subset.py <emitted.c> [-o out.c] [--report]
"""

import argparse
import re
import sys

# --------------------------------------------------------------------------
# prototype table: function name -> declared return type
# --------------------------------------------------------------------------
# The emitted C forward-declares every function it defines, so one linear scan
# over the file is enough; no ordering assumption is needed.
PROTO_RE = re.compile(
    r'^\s*(?:static\s+|extern\s+|inline\s+|__attribute__\(\([^)]*\)\)\s*)*'
    r'([A-Za-z_][A-Za-z0-9_ ]*?[A-Za-z0-9_])\s*(\**)\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\(')

NOT_A_TYPE = {'return', 'if', 'while', 'for', 'switch', 'sizeof', 'typedef',
              'else', 'do', 'case', 'goto'}

# The CPS-IR backend tags its call temps with a trailing block comment
# (`/* cps->direct */`, emit_cps_ir.c), so the terminating `;` is not always the
# last thing on the line.
AUTO_RE = re.compile(
    r'^(\s*)__auto_type\s+([A-Za-z0-9_]+)\s*=\s*(.*?);'
    r'(\s*(?:/\*.*?\*/\s*)*)$')

# `(T){0}` where T is scalar.  Pointer types (anything ending in `*`) are
# scalar too and get the same cast treatment; a bare struct tag does not match.
SCALAR_ZERO_RE = re.compile(
    r'\((\s*(?:bool|_Bool|char|short|int|long|float|double|unsigned|signed'
    r'|u?int(?:8|16|32|64|ptr)_t|size_t|ssize_t|ptrdiff_t'
    r'|[A-Za-z_][A-Za-z0-9_ ]*\*)\s*(?:\s*\*)*\s*)\)\{0\}')

# Indirect call through a cast function pointer: `((RET (*)(SIG))expr)(args)`.
FNPTR_CAST_RE = re.compile(
    r'^\(+\s*([A-Za-z_][A-Za-z0-9_ ]*?[A-Za-z0-9_])\s*(\**)\s*'
    r'\(\s*\*\s*\)\s*\(')

# Call through an emitted thunk typedef: `tur_thunk_<ret>_<args...>_t`.
THUNK_RE = re.compile(r'tur_thunk_([A-Za-z0-9_]+)_t')
THUNK_RETS = ('int64_t', 'double', 'bool', 'void')

# `<expr>.fn(` / `<expr>->fn(` -- a fat/poly closure dispatch member call.
MEMBER_FN_RE = re.compile(r'(?:\.|->)fn\s*\(')


def balanced(s):
    depth = 0
    for c in s:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def build_proto_table(lines):
    proto = {}
    for line in lines:
        m = PROTO_RE.match(line)
        if not m:
            continue
        ret, stars, name = m.group(1).strip(), m.group(2), m.group(3)
        if ret in NOT_A_TYPE:
            continue
        proto.setdefault(name, (ret + ' ' + stars).strip())
    return proto


def deduce_type(init, proto):
    """The C type of a parenthesized call expression, or None."""
    e = init.strip()
    while e.startswith('(') and e.endswith(')') and balanced(e[1:-1]):
        e = e[1:-1].strip()

    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*\(', e)
    if m and m.group(1) in proto:
        return proto[m.group(1)]

    m = FNPTR_CAST_RE.match(e)
    if m:
        return (m.group(1) + ' ' + m.group(2)).strip()

    m = THUNK_RE.search(e)
    if m:
        head = m.group(1)
        for ret in THUNK_RETS:
            if head == ret or head.startswith(ret + '_'):
                return ret

    # Dispatch through a fat/poly closure member: `f.fn(...)`, `p->lk.fn(...)`.
    # Every emitted dispatch struct that carries a value-producing `fn` member
    # (tur_poly_fn_t, tur_handler_t, tur_handler_entry_t, and the lifted env
    # structs) declares it `int64_t (*fn)(...)` -- the carrier ABI.  The `void
    # (*fn)(...)` members are drop glue, and a void call never reaches a hoist
    # site (emit_value returns before hoisting for TY_NIL/TY_NEVER), so a member
    # call that needs a type here is always the carrier.
    if MEMBER_FN_RE.search(e):
        return 'int64_t'
    return None


# --------------------------------------------------------------------------
# `__tur_include__` hoist -- NOT a subset fix
# --------------------------------------------------------------------------
# A faithful copy of hoist_tur_include_directives() (src/main.c:1756), which
# `tur build` runs over the emitted buffer but bare `tur emit-c` does not.
# Plan section 3.2 step 2 puts this post-pass on the JIT path too, so the spike
# has to do it or every fixture whose inline-C declares a file-scope type
# (httpd's HttpdConn, mbedTLS shims) fails with a bogus "unknown typedef".
INCLUDE_MARK_RE = re.compile(r'/\* __tur_include__: (.*?) \*/', re.S)


def hoist_tur_includes(text):
    bodies = INCLUDE_MARK_RE.findall(text)
    if not bodies:
        return text, 0
    # _DEFAULT_SOURCE must precede any hoisted system header -- see the comment
    # on the C original for why (feature-test macros lock in on first include).
    header = '#define _DEFAULT_SOURCE 1\n' + '\n'.join(bodies) + '\n'
    return header + text, len(bodies)


# --------------------------------------------------------------------------
# `__attribute__((constructor))` -- the load-bearing one
# --------------------------------------------------------------------------
# c2mir PARSES GCC attributes and then throws them away (c2mir.c:4392, "GCC
# attributes are not implemented"), with no diagnostic at the use site.  For
# `unused` that is harmless.  For `constructor` it is not: the emitted C uses
# it to register the direct->CPS function mapping (__tur_cps_register) and to
# create each dynamic variable's pthread key, both before main runs.  Dropped,
# the CPS registry stays empty -- an effectful indirect call then dispatches
# through NULL and the program takes SIGSEGV -- and dynamic variables silently
# read their root default instead of the innermost binding.
#
# There is no macro that recovers this, so the spike synthesizes the call
# sequence a real ELF ctor section would have run and invokes it at the top of
# main.  J1 wants this in the emitter (an explicit __tur_static_init() called
# from main) rather than in a rewriter: it is also the only way the JIT and the
# cc path can be guaranteed to agree on initialization order.
# The emitter spells the attribute in three positions -- ahead of the storage
# class, between `void` and the name, and as a separate declaration -- and
# sometimes on its own line above the definition.  Rather than encode one house
# style, a line is a candidate iff it mentions the attribute at all, and the
# function name is then pulled out separately.
CTOR_ATTR_RE = re.compile(r'__attribute__\(\(\s*constructor\s*\)\)')
CTOR_NAME_RE = re.compile(
    r'\bstatic\s+void\s+(?:__attribute__\(\([^)]*\)\)\s*)?'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*void\s*\)')
MAIN_RE = re.compile(r'^\s*int\s+main\s*\(')

CTOR_RUNNER = '__tur_jit_run_ctors'


def collect_ctors(lines):
    """Constructor function names, in the source order a ctor section uses."""
    names, seen, pending_attr = [], set(), False
    for line in lines:
        has_attr = CTOR_ATTR_RE.search(line) is not None
        if not has_attr and not pending_attr:
            continue
        m = CTOR_NAME_RE.search(line)
        if m is None:
            # a bare `__attribute__((constructor))` line: the definition is next
            pending_attr = has_attr
            continue
        pending_attr = False
        if m.group(1) not in seen:
            seen.add(m.group(1))
            names.append(m.group(1))
    return names


def inject_ctor_runner(lines, names):
    """Define a runner just above main and call it as main's first statement."""
    if not names:
        return lines, 0
    out, injected = [], False
    for line in lines:
        if not injected and MAIN_RE.match(line):
            out.append('static void %s(void) {' % CTOR_RUNNER)
            out.extend('    %s();' % n for n in names)
            out.append('}')
            out.append(line)
            # main's `{` is on the same line in the emitted C.
            if line.rstrip().endswith('{'):
                out.append('    %s();' % CTOR_RUNNER)
                injected = True
                continue
            injected = True
            continue
        out.append(line)
    return out, len(names)


def normalize(text):
    text, n_hoisted = hoist_tur_includes(text)
    lines = text.split('\n')
    proto = build_proto_table(lines)
    out, unresolved = [], []
    n_auto = n_zero = 0

    for lineno, line in enumerate(lines, 1):
        fixed = SCALAR_ZERO_RE.sub(
            lambda m: '((%s)0)' % m.group(1).strip(), line)
        if fixed != line:
            n_zero += len(SCALAR_ZERO_RE.findall(line))
        line = fixed

        m = AUTO_RE.match(line)
        if m is None:
            out.append(line)
            continue
        ty = deduce_type(m.group(3), proto)
        if ty is None:
            unresolved.append((lineno, line.strip()[:140]))
            out.append(line)
        else:
            n_auto += 1
            out.append('%s%s %s = %s;%s'
                       % (m.group(1), ty, m.group(2), m.group(3), m.group(4)))

    out, n_ctors = inject_ctor_runner(out, collect_ctors(lines))

    return '\n'.join(out), {'auto_type': n_auto, 'scalar_zero': n_zero,
                            'hoisted': n_hoisted, 'ctors': n_ctors,
                            'unresolved': unresolved, 'protos': len(proto)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source')
    ap.add_argument('-o', '--out', default='-')
    ap.add_argument('--report', action='store_true',
                    help='print a rewrite tally to stderr')
    args = ap.parse_args()

    with open(args.source) as f:
        text = f.read()
    result, stats = normalize(text)

    if args.report:
        sys.stderr.write(
            '%s: %d __auto_type, %d scalar (T){0}, %d hoisted __tur_include__,'
            ' %d constructors, %d prototypes\n'
            % (args.source, stats['auto_type'], stats['scalar_zero'],
               stats['hoisted'], stats['ctors'], stats['protos']))
    if stats['unresolved']:
        sys.stderr.write('%s: %d UNRESOLVED __auto_type site(s)\n'
                         % (args.source, len(stats['unresolved'])))
        for lineno, snippet in stats['unresolved'][:10]:
            sys.stderr.write('  %d: %s\n' % (lineno, snippet))

    if args.out == '-':
        sys.stdout.write(result)
    else:
        with open(args.out, 'w') as f:
            f.write(result)
    return 1 if stats['unresolved'] else 0


if __name__ == '__main__':
    sys.exit(main())
