#!/usr/bin/env python3
"""
tests/tools/check_gendocs_parse.py -- tools/gendocs.py must read a defn
signature the same way whichever annotation spelling it uses.

gendocs-misparses-the-spaced-annotation-form: the parameter vector was split
on whitespace and each ':'-prefixed token taken as the previous name's type.
That reads the FUSED form `[a :int]` correctly, but in the SPACED form
`[a : int]` -- which 612 stdlib defns and CLAUDE.md's own style guide use --
the colon is its own token, so `a` got the type ':' and a phantom parameter
named `int` appeared; the return position `] : int` came back as ':'.  53% of
the stdlib would have rendered with a wrong signature on the next
`tur run docs`.

Runs without the `markdown` package (a stub stands in for it), because the
parser is what is under test, not the HTML renderer.
"""
import os
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.modules.setdefault('markdown', types.ModuleType('markdown'))

from gendocs import _parse_def_line, _extract_return_type  # noqa: E402

CASES = [
    # (kind, text, params, return_type)
    ('defn', '(defn g [a :int b :int] :int (+ a b))',
     [('a', ':int'), ('b', ':int')], ':int'),                       # fused
    ('defn', '(defn g [a : int b : int] : int (+ a b))',
     [('a', ':int'), ('b', ':int')], ':int'),                       # spaced
    ('defn', '(defn g [a b] (+ a b))',
     [('a', None), ('b', None)], None),                             # untyped
    ('defn', '(defn arc-new [v : int] : Arc ...)',
     [('v', ':int')], ':Arc'),                                      # stdlib/arc.tur
    ('defn', '(defn greet [name : cstr] : void (println name))',
     [('name', ':cstr')], ':void'),                                 # CLAUDE.md style
    ('defn', '(defn f [x : int] #fx{Unsafe} : int x)',
     [('x', ':int')], ':int'),                                      # effect row
    ('defn', '(defn h [o : (Option int) k : ptr<Foo>] : (Result int cstr) o)',
     [('o', ':(Option int)'), ('k', ':ptr<Foo>')], ':(Result int cstr)'),  # compound
    ('defn', '(defn ap [f : (fn [int] int) x : int] : int (f x))',
     [('f', ':(fn [int] int)'), ('x', ':int')], ':int'),           # nested vector
    ('defn', '(defn v [first : cstr & rest : cstr] : void first)',
     [('first', ':cstr'), ('rest', '... :cstr')], ':void'),      # variadic, spaced
    ('defn', '(defn m [^mut v : int] : int v)',
     [('^mut v', ':int')], ':int'),                                 # UT3 annotation
    ('defstruct', '(defstruct Pt [x : float y : float])',
     [('x', ':float'), ('y', ':float')], None),                    # struct fields
]

fails = 0
for kind, text, want_params, want_ret in CASES:
    name, params, ret, _extra = _parse_def_line(kind, text)
    if params != want_params or ret != want_ret:
        fails += 1
        print(f"FAIL gendocs-parse: {text}")
        print(f"     params: got {params!r}, want {want_params!r}")
        print(f"     return: got {ret!r}, want {want_ret!r}")

for line, want in [('(defn g [a : int] : int', ':int'),
                   ('(defn g [a :int] :int', ':int'),
                   ('(defn g [a]', None)]:
    got = _extract_return_type(line)
    if got != want:
        fails += 1
        print(f"FAIL gendocs-parse: _extract_return_type({line!r}) = {got!r}, want {want!r}")

if fails:
    print(f"gendocs-parse: {fails} case(s) failed")
    sys.exit(1)
print(f"PASS check-gendocs-parse ({len(CASES)} signatures, both spellings)")
