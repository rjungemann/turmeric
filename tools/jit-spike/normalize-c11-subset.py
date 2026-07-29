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
import os
import re
import sys

# --------------------------------------------------------------------------
# prototype table: function name -> declared return type
# --------------------------------------------------------------------------
# The emitted C forward-declares every function it defines, so one linear scan
# over the file is enough; no ordering assumption is needed.
# The separator between return type and name is MANDATORY (whitespace and/or
# `*`).  With it optional, a plain CALL statement `snprintf(__m, ...)` matched
# as return type `sn` + name `printf` -- the lazy type group happily splits a
# single identifier -- and poisoned the table with `printf -> sn`, which then
# emitted `sn __ps_N = (printf(...));` and failed 24 fixtures as opaque parse
# errors from the very first sweep.  Any identifier that splits two ways did
# this; the mandatory separator makes the split impossible.
PROTO_RE = re.compile(
    r'^\s*(?:static\s+|extern\s+|inline\s+|__attribute__\(\([^)]*\)\)\s*)*'
    r'([A-Za-z_][A-Za-z0-9_ ]*?[A-Za-z0-9_])'
    r'((?:\s|\*)+)'
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

# Call through an emitted function-pointer typedef: `(*( SomeTypedef *)(x))(..)`.
# The typedef's return type is READ from its own definition in the same TU --
# `typedef <ret> (*<name>)(...)` -- rather than guessed from the name.  The old
# name-prefix heuristic (int64_t/double/bool/void) silently failed on aggregate
# thunks like tur_thunk_tur_adt_Person_..._t, whose return type is a struct.
THUNK_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*_t)\b')
TYPEDEF_FNPTR_RE = re.compile(
    r'^\s*typedef\s+([A-Za-z_][A-Za-z0-9_ ]*?\**)\s*'
    r'\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(')

# A fat/poly closure dispatch member call, ANCHORED at the start of the
# (paren-stripped) expression: `g.fn(...)`, `__env_1->lk.fn(...)`.
#
# Anchoring is the whole point.  An unanchored search matches the member call
# nested inside a deref-of-cast --
#   *(tur_adt_Option__int *)(intptr_t)(g.fn(g.env, ...))
# -- whose value is the STRUCT, not the carrier the callee returned, and typing
# that as int64_t makes c2mir reject the assignment ("incompatible types in
# assignment to an arithmetic type lvalue").  Caught on hrt-rank2-aggregate-arg
# and hrt-hkt-aggregate-container.
# `INT64_C(n)` / `UINT64_C(n)` -- stdint's integer-constant macros.  Exact: the
# macro's whole purpose is to give the literal that type.
INTC_RE = re.compile(r'^U?INT(8|16|32|64)_C\s*\(')

# `TUR_APPLY<N>_T(R, A0.., f, a..)` -- the emitted typed-apply macro.  Its FIRST
# argument is the return type, by its own definition in the preamble, so this is
# a read rather than an inference.
TUR_APPLY_RE = re.compile(r'^TUR_APPLY\d+_T\s*\(\s*([^,]+?)\s*,')

# `(T)(expr)` -- an ordinary cast; the type is written right there.  Restricted
# to a recognized primitive spelling OR anything ending in `*`, so that `(f)(x)`
# -- a call through a parenthesized function name -- cannot be mistaken for one.
CAST_RE = re.compile(
    r'^\(\s*((?:const\s+)?(?:bool|_Bool|char|short|int|long|float|double'
    r'|unsigned|signed|u?int(?:8|16|32|64|ptr)_t|size_t|ssize_t|ptrdiff_t)'
    r'(?:\s*\*)*|[A-Za-z_][A-Za-z0-9_ ]*\*+)\s*\)\s*[\(A-Za-z_]')

# `*(T *)(...)` -- unboxing a carrier back to an aggregate.  Exact, not a guess.
DEREF_CAST_RE = re.compile(r'^\*\s*\(\s*([A-Za-z_][A-Za-z0-9_ ]*?)\s*\*\s*\)')

MEMBER_FN_RE = re.compile(
    r'^[A-Za-z_][A-Za-z0-9_]*'
    r'(?:(?:\.|->)[A-Za-z_][A-Za-z0-9_]*)*'
    r'(?:\.|->)fn\s*\(')


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
        ret, sep, name = m.group(1).strip(), m.group(2), m.group(3)
        if ret in NOT_A_TYPE:
            continue
        stars = '*' * sep.count('*')
        proto.setdefault(name, (ret + ' ' + stars).strip())
    return proto


def build_fnptr_typedef_table(lines):
    """typedef name -> declared return type, for `typedef R (*name)(...)`."""
    table = {}
    for line in lines:
        m = TYPEDEF_FNPTR_RE.match(line)
        if m:
            table.setdefault(m.group(2), ' '.join(m.group(1).split()))
    return table


def deduce_type(init, proto, fnptr_typedefs):
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

    for m in THUNK_RE.finditer(e):
        if m.group(1) in fnptr_typedefs:
            return fnptr_typedefs[m.group(1)]

    # Dispatch through a fat/poly closure member: `f.fn(...)`, `p->lk.fn(...)`.
    # Every emitted dispatch struct that carries a value-producing `fn` member
    # (tur_poly_fn_t, tur_handler_t, tur_handler_entry_t, and the lifted env
    # structs) declares it `int64_t (*fn)(...)` -- the carrier ABI.  The `void
    # (*fn)(...)` members are drop glue, and a void call never reaches a hoist
    # site (emit_value returns before hoisting for TY_NIL/TY_NEVER), so a member
    # call that needs a type here is always the carrier.
    if MEMBER_FN_RE.match(e):
        return 'int64_t'

    # Deref of a cast: `*(T *)(...)` has type `T`, exactly -- no guessing.  This
    # is how a carrier-returning dispatch is unboxed back to an aggregate, e.g.
    #   *(tur_adt_Option__int *)(intptr_t)(g.fn(g.env, ...))
    m = DEREF_CAST_RE.match(e)
    if m:
        return m.group(1).strip()

    m = TUR_APPLY_RE.match(e)
    if m:
        return m.group(1).strip()

    m = INTC_RE.match(e)
    if m:
        return ('u' if e[0] == 'U' else '') + 'int' + m.group(1) + '_t'

    m = CAST_RE.match(e)
    if m:
        return ' '.join(m.group(1).split())
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


QUOTED_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def scan_quoted_includes(lines, include_dirs):
    """Prototype lines from project headers the TU includes by quote.

    The `#include "hamt.h"` path (g_needs_hamt) declares the whole HAMT API in
    a header rather than as inline externs, so the TU text alone has no
    prototype for `tur_hamt_new` et al. and every hoisted call to them stayed
    on __auto_type.  Reading the header is still a read, not a guess -- these
    are the same declarations the C compiler resolves the calls against.
    Angle includes are system headers and are deliberately not scanned."""
    out = []
    for line in lines:
        m = QUOTED_INCLUDE_RE.match(line)
        if not m:
            continue
        for d in include_dirs:
            path = os.path.join(d, m.group(1))
            if os.path.isfile(path):
                with open(path) as f:
                    out.extend(f.read().split('\n'))
                break
    return out


def normalize(text, include_dirs=()):
    text, n_hoisted = hoist_tur_includes(text)
    lines = text.split('\n')
    proto_lines = lines + scan_quoted_includes(lines, include_dirs)
    proto = build_proto_table(proto_lines)
    fnptr_typedefs = build_fnptr_typedef_table(proto_lines)
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
        ty = deduce_type(m.group(3), proto, fnptr_typedefs)
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
    ap.add_argument('-I', '--include-dir', action='append', default=[],
                    help='scan quoted #include files here for prototypes')
    args = ap.parse_args()

    with open(args.source) as f:
        text = f.read()
    result, stats = normalize(text, args.include_dir)

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
