#!/usr/bin/env python3
"""tests/saffron-fuzz-src.py -- SOURCE-LEVEL differential fuzzer for Saffron.

Why this exists
---------------
`tests/type-fuzz-src.py` proved that a correct-by-construction generator with
a total oracle finds the bugs hand-written fixtures miss: it routes a known
value through random plumbing and asserts the program prints what the
generator predicted.  This harness applies the same pattern to `#lang
saffron`, where the plumbing is the `any` box: an unannotated parameter or
return, a container element, a dynamic method witness, the checked seam into
a typed callee, an `is?` narrow.  The pass recorded in
docs/reported/saffron-dynamic-surface-pass.md found its defects along exactly
three axes -- the KIND a box holds, the ROUTE it takes, and the OPERATION
applied at the end -- and every one of them was a combination nobody had
written a fixture for.  The generator walks that product.

The property that matters
-------------------------
Every generated program is CORRECT BY CONSTRUCTION: the generator tracks the
concrete value it routes through the boxes and knows the exact stdout the
program must produce.  Saffron ships on two back ends, so the oracle is
three-way:

    tur check accepts  ==>  the C compiles, links, runs cleanly and prints
                            the predicted output, AND `tur --interpret`
                            prints the same predicted output

The compiled arm classifies as in type-fuzz-src:

  * BUG_invalid_c      -- `tur check` passed, cc rejected the emitted C
  * BUG_link           -- cc compiled, the link failed
  * BUG_crash          -- the binary died (SIGSEGV/SIGBUS/SIGABRT, or a panic)
  * BUG_wrong_output   -- ran clean, printed the wrong values

and the interpreter arm adds:

  * IBUG_abort         -- `tur --interpret` exited non-zero (panic, unbound
                          variable, evaluator error)
  * IBUG_wrong_output  -- ran clean, printed the wrong values

A case whose compiled arm is clean and whose interpreter arm is not is a
back-end DIVERGENCE by construction; the IBUG_ prefix is that flag.  One
report-only class, GEN_REJECT, is a `tur check` rejection of a program the
generator claims is legal -- saved for triage, never failed on.

What it generates
-----------------
A program is 1-3 independent LEGS.  Each leg takes a known scalar (int, a
float with a non-zero fraction, bool, cstr), WRAPS it (bare, `(Vec any)`
element, `(Option any)`, a parametric user ADT field, a typed struct field,
a `(Map Sym any)` value, a zero-arg thunk), pushes the wrapped box through
1-3 ROUTES while it is an `any`:

    pass-through defn / two-level defn / let-in-defn / self-recursive defn
    non-capturing HOF / `is?`-narrow-and-rewiden / checked seam into a typed
    callee and back out

then UNWRAPS it (vec-get, unwrap-or, match, dynamic field read, map-get,
dynamic zero-arg call), optionally applies one TERMINAL dynamic operation
(arithmetic, comparison, truthiness, a user typeclass method dispatched on
the box tag, a runtime refinement contract, `fmap` over the Option), and
prints `(type-of x)` and `x` through an unannotated sink.  Floats are chosen
so `%g` prints them exactly, and the `type-of` line is what makes an
int-for-float payload confusion visible even when the digits agree.

Known open bugs
---------------
Shapes that reproduce the open findings in
docs/reported/saffron-dynamic-surface-pass.md are excluded from the default
pool -- a run's value is what it finds that is NOT yet on file.
`known_bug_slug()` is the single place that knowledge lives; `--emit-known`
re-enables those shapes (classified KNOWN_<finding>, never failing the run)
and `--known-probes` runs one pinned repro per finding and prints
fires/fixed, so the avoid list cannot silently rot: a `fixed` row means the
row should be retired here and its shape returned to the default pool.

Attribution
-----------
A failing program is auto-bisected: each leg is rerun alone and the failing
leg(s) are saved with their shape tags, so a finding reads as
"scalar_float wrap_opt route_seam,route_rec term_class" rather than a blob.
If no leg fails alone the whole program is saved as an INTERACTION finding.

Proving the fuzzer can fail
---------------------------
`--self-test` feeds the classifier fixed programs: a clean pass, a wrong
output, a compiled crash, a checker rejection, and an interpreter-arm
failure.  Every classification is asserted.  Beyond plumbing, `--known-probes`
must show every open finding firing on an unfixed build.

Usage
-----
    python3 tests/saffron-fuzz-src.py [--n 200] [--seed 1] [--jobs N]
                                      [--tur ./build/tur] [--save-dir DIR]
                                      [--legs 1..3] [--emit-known]
                                      [--self-test] [--known-probes]

Exit status is 1 if any BUG_/IBUG_ class fired (GEN_REJECT and KNOWN_* never
fail the run), 0 otherwise.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TIMEOUT = 90

# ---------------------------------------------------------------------------
# Known open findings -- the single place this knowledge lives.
#
# Each row: (finding id in docs/reported/saffron-dynamic-surface-pass.md, tag).
# A leg carrying the tag is (a) not generated by default and (b) classified
# KNOWN(<id>) when --emit-known turns it back on.  When a finding is fixed,
# delete its row and the shape returns to the default pool.
# ---------------------------------------------------------------------------

KNOWN = [
    # H1-capturing-lambda (route_capture): retired 2026-09-10.
    ("H5-sym-in-any",            ("scalar_sym",)),
    # H6-forward-ref-int (route_fwdref): retired 2026-09-10.
    ("H7-seam-fn-param",         ("route_seam_fn",)),
    ("H8-typed-defn-in-any",     ("route_typed_fn_value",)),
    ("H9-any-map-into-map-get",  ("wrap_map_outer",)),
    # H10-lambda-literal-body (wrap_thunk_lit): retired 2026-09-10.
    # H11-field-read-no-inst-rows (wrap_struct + term_class): retired 2026-09-10.
    ("M7-dynamic-cons-field",    ("wrap_cons",)),
    ("M10-macro-any-not-seamed", ("wrap_map_inner", "seam_first")),
    ("L-ctor-under-typed-expected", ("wrap_adt", "seam_first")),
]


def known_bug_slug(tags):
    """A row matches when ALL its tags are present; a leg can match more
    than one row."""
    hits = sorted(_id for _id, need in KNOWN if all(t in tags for t in need))
    return "+".join(hits) if hits else None


def known_shape(tags):
    return known_bug_slug(tags) is not None


# ---------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------

FLOATS = [0.25, 0.75, 1.5, 2.25, 3.5, 7.25, 12.75, 33.5, 100.125, 0.125,
          -3.5, -7.25, 250.75]
CSTRS = ["s", "ay", "bee", "hello", "x1", "z"]


class V:
    """A scalar the generator is routing, with its source literal, the
    `println` form, and the `type-of` name."""

    def __init__(self, kind, val):
        self.kind = kind
        self.val = val

    @property
    def lit(self):
        if self.kind == "int":
            return str(self.val)
        if self.kind == "float":
            return repr(float(self.val))
        if self.kind == "bool":
            return "true" if self.val else "false"
        if self.kind == "cstr":
            return '"%s"' % self.val
        if self.kind == "sym":
            return ":" + self.val
        raise AssertionError(self.kind)

    @property
    def out(self):
        if self.kind == "float":
            return "%g" % self.val
        if self.kind == "cstr":
            return self.val
        if self.kind == "sym":
            return ":" + self.val
        return self.lit

    @property
    def tname(self):
        return {"int": "int", "float": "float", "bool": "bool",
                "cstr": "cstr", "sym": "Sym"}[self.kind]

    def positive(self):
        return self.kind in ("int", "float") and self.val > 0


def pick_scalar(rng, emit_known):
    kinds = ["int", "float", "bool", "cstr"]
    if emit_known:
        kinds.append("sym")
    k = rng.choice(kinds)
    if k == "int":
        return V(k, rng.randint(-40, 400))
    if k == "float":
        return V(k, rng.choice(FLOATS))
    if k == "bool":
        return V(k, rng.random() < 0.5)
    if k == "cstr":
        return V(k, rng.choice(CSTRS))
    return V(k, rng.choice(["kw", "a", "tag"]))


# ---------------------------------------------------------------------------
# Program pieces
# ---------------------------------------------------------------------------

PRELUDE = """#lang saffron
;; generated by tests/saffron-fuzz-src.py

(defclass Kind [a]
  (kind-of [x] : cstr))
(definstance Kind [int]   (kind-of [x] "I"))
(definstance Kind [float] (kind-of [x] "F"))
(definstance Kind [cstr]  (kind-of [x] "C"))
(definstance Kind [bool]  (kind-of [x] "B"))

(defn show  [x]   (println (type-of x)) (println x))
(defn call0 [f]   (f))
(defn app   [f x] (f x))
"""


class Leg:
    def __init__(self):
        self.defs = []       # emitted in order, before main
        self.late_defs = []  # emitted AFTER the leg's other defs (fwdref)
        self.body = []
        self.expected = []
        self.tags = set()


class Gen:
    def __init__(self, rng, idx, emit_known):
        self.rng = rng
        self.idx = idx
        self.emit_known = emit_known
        self.n = 0

    def name(self, base):
        # No underscore: a parametric ADT constructor with `_` in its name
        # is mangled two ways and the C does not compile
        # (docs/reported/adt-ctor-underscore-mangles-twice.md) -- a real
        # finding of this harness's first run, but not a Saffron one, so the
        # generator steps around it rather than reporting it 25% of the time.
        self.n += 1
        return "%s%dx%d" % (base, self.idx, self.n)

    def allow(self, tag):
        return self.emit_known or not known_shape({tag})

    # -- wrappers ------------------------------------------------------------
    #
    # Each returns (wrap_expr, static_type_or_None, unwrap_fn) where
    # unwrap_fn(expr) yields the scalar back as an `any`.

    def wrappers(self, v):
        leg = self.leg
        ws = []

        ws.append(("wrap_bare", lambda e: e, v.tname, lambda e: e))

        def w_vec(e):
            pad = self.rng.choice(["7", '"pad"', "1.5", "false"])
            if self.rng.random() < 0.5:
                self.vec_idx = 0
                return "[%s %s]" % (e, pad)
            self.vec_idx = 1
            return "[%s %s]" % (pad, e)
        ws.append(("wrap_vec", w_vec, "(Vec any)",
                   lambda e: "(vec-get %s %d)" % (e, self.vec_idx)))

        ws.append(("wrap_opt", lambda e: "(some %s)" % e, "(Option any)",
                   lambda e: "(unwrap-or (cast %s (Option any)) (:: 0 any))" % e))

        adt = self.name("W")
        leg.defs.append("(defdata %s [a] (Wrap%s a))" % (adt, adt))
        ws.append(("wrap_adt", lambda e: "(Wrap%s %s)" % (adt, e),
                   "(%s any)" % adt,
                   lambda e: "(match %s (Wrap%s v) v)" % (e, adt)))

        st = self.name("P")
        leg.defs.append("(defstruct %s [fld : %s])" % (st, v.tname))
        ws.append(("wrap_struct", lambda e: "(make-struct %s %s)" % (st, e),
                   st, lambda e: "(.fld %s)" % e))

        # A thunk whose body flows through an `any`: the literal-body form is
        # finding H10 (a concrete lambda return is not dynamically callable).
        thru = self.thru = self.name("thru")
        leg.defs.append("(defn %s [x] x)" % thru)
        ws.append(("wrap_thunk", lambda e: "(fn [] (%s %s))" % (thru, e), None,
                   lambda e: "(call0 %s)" % e))
        if self.allow("wrap_thunk_lit"):
            ws.append(("wrap_thunk_lit", lambda e: "(fn [] %s)" % e, None,
                       lambda e: "(call0 %s)" % e))

        # `#map{...}` values: the map itself must stay in the typed scope it
        # was built in (H9 is the any-held map), so this wrapper unwraps
        # immediately and the routes run on the extracted value.
        ws.append(("wrap_map_inner",
                   lambda e: "(map-get #map{:k %s :other 1} :k)" % e,
                   v.tname, lambda e: e))
        if self.allow("wrap_map_outer"):
            ws.append(("wrap_map_outer", lambda e: "#map{:k %s}" % e,
                       "(Map Sym any)", lambda e: "(map-get %s :k)" % e))
        if self.allow("wrap_cons"):
            ws.append(("wrap_cons", lambda e: "(list %s 1)" % e, "(Cons any)",
                       lambda e: "(.head %s)" % e))
        return ws

    # -- routes --------------------------------------------------------------
    #
    # Each takes the current expression (an `any`) and returns the new one.

    def route(self, e, stype, wtag):
        leg = self.leg
        rng = self.rng
        choices = ["through", "deep", "let", "rec", "hof", "narrow", "seam"]
        if self.allow("route_fwdref"):
            choices.append("fwdref")
        if self.allow("route_capture"):
            choices.append("capture")
        if self.allow("route_typed_fn_value"):
            choices.append("typed_fn_value")
        if self.allow("route_seam_fn") and wtag.startswith("wrap_thunk"):
            choices.append("seam_fn")
        r = rng.choice(choices)
        leg.tags.add("route_" + r)

        if r == "through":
            f = self.name("t")
            leg.defs.append("(defn %s [x] x)" % f)
            return "(%s %s)" % (f, e)
        if r == "deep":
            f, g = self.name("t"), self.name("d")
            leg.defs.append("(defn %s [x] x)" % f)
            leg.defs.append("(defn %s [x] (%s x))" % (g, f))
            return "(%s %s)" % (g, e)
        if r == "let":
            f = self.name("l")
            leg.defs.append("(defn %s [x] (let [y x] y))" % f)
            return "(%s %s)" % (f, e)
        if r == "rec":
            f = self.name("r")
            leg.defs.append("(defn %s [x n] (if (= n 0) x (%s x (- n 1))))"
                            % (f, f))
            return "(%s %s %d)" % (f, e, rng.randint(1, 4))
        if r == "hof":
            f = self.name("h")
            leg.defs.append("(defn %s [f x] (f x))" % f)
            return "(%s (fn [y] y) %s)" % (f, e)
        if r == "narrow":
            if stype is None:
                leg.tags.discard("route_narrow")
                return e
            f = self.name("n")
            leg.defs.append("(defn %s [x] (if (is? x %s) (:: x any) x))"
                            % (f, stype))
            return "(%s %s)" % (f, e)
        if r == "seam":
            if stype is None:
                leg.tags.discard("route_seam")
                return e
            f = self.name("s")
            leg.defs.append("(defn %s [v : %s] : %s v)" % (f, stype, stype))
            # The typed result is re-boxed by passing it back through an
            # unannotated defn: the unwrap that follows may `cast`, which
            # refuses a typed operand.
            return "(%s (%s %s))" % (self.thru, f, e)
        if r == "seam_fn":
            f = self.name("sf")
            leg.defs.append("(defn %s [f : (fn [] any)] : any (f))" % f)
            return "(%s %s)" % (f, e)
        if r == "fwdref":
            f = self.name("fw")
            leg.late_defs.append("(defn %s [x] x)" % f)
            g = self.name("fu")
            leg.defs.append("(defn %s [x] (%s x))" % (g, f))
            return "(%s %s)" % (g, e)
        if r == "capture":
            f = self.name("cap")
            leg.defs.append("(defn %s [v] (fn [] v))" % f)
            return "(call0 (%s %s))" % (f, e)
        if r == "typed_fn_value":
            if stype is None:
                leg.tags.discard("route_typed_fn_value")
                return e
            f = self.name("tf")
            leg.defs.append("(defn %s [v : %s] : %s v)" % (f, stype, stype))
            return "(app %s %s)" % (f, e)
        raise AssertionError(r)

    # -- terminals -----------------------------------------------------------

    def terminal(self, e, v):
        leg = self.leg
        rng = self.rng
        choices = ["none", "class", "truthy"]
        if v.kind == "int":
            choices += ["arith_int", "cmp"]
        if v.kind == "float":
            choices += ["arith_float", "cmp"]
        if v.kind == "bool":
            choices += ["cmp"]
        if v.positive():
            choices.append("refine")
        t = rng.choice(choices)
        if t != "none":
            leg.tags.add("term_" + t)

        if t == "none":
            return e, v
        if t == "class":
            f = self.name("k")
            leg.defs.append("(defn %s [x] (.kind-of x))" % f)
            return "(%s %s)" % (f, e), V("cstr", v.tname[0].upper())
        if t == "truthy":
            f = self.name("tr")
            leg.defs.append('(defn %s [x] (if x "t" "f"))' % f)
            falsy = v.kind == "bool" and v.val is False
            return "(%s %s)" % (f, e), V("cstr", "f" if falsy else "t")
        if t == "arith_int":
            op, k = rng.choice([("+", 1), ("*", 2), ("-", 3), ("+", 10)])
            f = self.name("a")
            leg.defs.append("(defn %s [x] (%s x %d))" % (f, op, k))
            val = {"+": v.val + k, "*": v.val * k, "-": v.val - k}[op]
            return "(%s %s)" % (f, e), V("int", val)
        if t == "arith_float":
            op, k = rng.choice([("+", "0.5"), ("*", "2"), ("+", "1"),
                                ("-", "0.25")])
            f = self.name("a")
            leg.defs.append("(defn %s [x] (%s x %s))" % (f, op, k))
            kf = float(k)
            val = {"+": v.val + kf, "*": v.val * kf, "-": v.val - kf}[op]
            return "(%s %s)" % (f, e), V("float", val)
        if t == "cmp":
            f = self.name("c")
            if v.kind == "bool":
                leg.defs.append("(defn %s [x] (= x %s))" % (f, v.lit))
                return "(%s %s)" % (f, e), V("bool", True)
            if rng.random() < 0.5:
                leg.defs.append("(defn %s [x] (< x 100000))" % f)
            else:
                leg.defs.append("(defn %s [x] (= x %s))" % (f, v.lit))
            return "(%s %s)" % (f, e), V("bool", True)
        if t == "refine":
            f = self.name("p")
            leg.defs.append("(defn %s [n : #refine{v : any | (> v 0)}] n)" % f)
            return "(%s %s)" % (f, e), v
        raise AssertionError(t)

    # -- one leg -------------------------------------------------------------

    def leg_gen(self):
        # Composite known shapes (a wrapper AND a route/terminal) are only
        # recognisable once the leg is built, so redraw until it is clean.
        for _ in range(20):
            leg = self._leg_gen()
            if self.emit_known or not known_shape(leg.tags):
                return leg
        return leg

    def _leg_gen(self):
        rng = self.rng
        self.leg = leg = Leg()
        v = pick_scalar(rng, self.emit_known)
        leg.tags.add("scalar_" + v.kind)

        wtag, wrap, stype, unwrap = rng.choice(self.wrappers(v))
        leg.tags.add(wtag)
        e = wrap(v.lit)

        for i in range(rng.randint(1, 3)):
            before = set(leg.tags)
            e = self.route(e, stype, wtag)
            if i == 0 and "route_seam" in leg.tags - before:
                leg.tags.add("seam_first")

        # An Option can be mapped over before it is opened.
        if wtag == "wrap_opt" and rng.random() < 0.5:
            f = self.name("fm")
            if v.kind in ("int", "float"):
                leg.defs.append("(defn %s [o] (.fmap o (fn [x] (+ x 1))))" % f)
                v = V(v.kind, v.val + 1)
            else:
                leg.defs.append("(defn %s [o] (.fmap o (fn [x] x)))" % f)
            leg.tags.add("term_fmap")
            e = "(%s %s)" % (f, e)

        e = unwrap(e)
        e, v = self.terminal(e, v)

        leg.body.append("(show %s)" % e)
        leg.expected.append(v.tname)
        leg.expected.append(v.out)
        return leg


def gen_program(rng, n_legs, emit_known):
    return [Gen(rng, i, emit_known).leg_gen() for i in range(n_legs)]


def assemble(legs):
    defs, late, body, expected = [], [], [], []
    for leg in legs:
        defs += leg.defs
        late += leg.late_defs
        body += leg.body
        expected += leg.expected
    src = PRELUDE + "\n" + "\n".join(defs) + "\n"
    src += "\n(defn main [] : int\n  " + "\n  ".join(body) + "\n  0)\n"
    if late:
        src += "\n" + "\n".join(late) + "\n"
    return src, "\n".join(expected) + "\n"


# ---------------------------------------------------------------------------
# Running one case
# ---------------------------------------------------------------------------

class Outcome:
    def __init__(self, kind, stdout="", stderr="", ikind=None, istdout="",
                 istderr=""):
        self.kind = kind          # compiled arm: clean/crash/invalid_c/link/reject/timeout/other
        self.stdout = stdout
        self.stderr = stderr
        self.ikind = ikind        # interpreter arm: clean/abort/timeout/None
        self.istdout = istdout
        self.istderr = istderr


def _errs(text):
    """The diagnostic lines of a stderr blob, ahead of its tail: the tail
    alone is usually the `tur_hamt_box_key` const-qualifier warning noise."""
    keep = [ln for ln in text.splitlines()
            if " error" in ln or "panic" in ln or ln.startswith("tur:")]
    return "\n".join(keep[:12]) + "\n...\n" + text[-1200:]


def _env():
    env = dict(os.environ)
    # Drop any ambient TUR_STDLIB_DIR so the compiler under test resolves the
    # stdlib beside it, not one from another install re-exported by a version
    # manager's python shim.  See tests/type-fuzz-src.py and
    # docs/archive/type-fuzz-src-red-on-clang-21.md for the full story.
    env.pop("TUR_STDLIB_DIR", None)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") or "detect_leaks=0"
    return env


def run_case(tur, path, src):
    with open(path, "w") as f:
        f.write(src)
    env = _env()
    try:
        chk = subprocess.run([tur, "check", path], capture_output=True,
                             text=True, timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")
    if chk.returncode != 0:
        return Outcome("reject", chk.stdout, chk.stderr)
    try:
        p = subprocess.run([tur, "run", path], capture_output=True,
                           text=True, timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")
    if p.returncode == 0:
        kind = "clean"
    elif p.returncode in (134, 138, 139) or p.returncode < 0:
        kind = "crash"
    else:
        blob = p.stderr + p.stdout
        if "cc invocation failed" in blob:
            if "undefined" in blob.lower() or "ld:" in blob:
                kind = "link"
            else:
                kind = "invalid_c"
        elif "panic" in blob:
            kind = "crash"
        else:
            kind = "other"
    try:
        i = subprocess.run([tur, "--interpret", path], capture_output=True,
                           text=True, timeout=TIMEOUT, cwd=REPO, env=env)
        ikind = "clean" if i.returncode == 0 else "abort"
        istdout, istderr = i.stdout, i.stderr
    except subprocess.TimeoutExpired:
        ikind, istdout, istderr = "timeout", "", ""
    return Outcome(kind, p.stdout, p.stderr, ikind, istdout, istderr)


BUG_OF = {"crash": "BUG_crash", "invalid_c": "BUG_invalid_c",
          "link": "BUG_link", "other": "BUG_toolchain_other"}


def classify(out, expected):
    if out.kind == "timeout":
        return "skip_timeout"
    if out.kind == "reject":
        return "GEN_REJECT"
    if out.kind == "clean":
        if out.stdout != expected:
            return "BUG_wrong_output"
    else:
        return BUG_OF[out.kind]
    # Compiled arm is clean and right; now the interpreter arm.
    if out.ikind == "timeout":
        return "skip_timeout"
    if out.ikind == "abort":
        return "IBUG_abort"
    if out.istdout != expected:
        return "IBUG_wrong_output"
    return "ok"


def one_case(tur, workdir, idx, seed, max_legs, emit_known):
    rng = random.Random(seed)
    legs = gen_program(rng, rng.randint(1, max_legs), emit_known)
    src, expected = assemble(legs)
    path = os.path.join(workdir, "c%06d.tur" % idx)
    out = run_case(tur, path, src)
    kind = classify(out, expected)

    detail = None
    if kind.startswith(("BUG", "IBUG")) or kind == "GEN_REJECT":
        failing = []
        for j, leg in enumerate(legs):
            s2, e2 = assemble([leg])
            p2 = os.path.join(workdir, "c%06d_leg%d.tur" % (idx, j))
            o2 = run_case(tur, p2, s2)
            k2 = classify(o2, e2)
            if k2 != "ok":
                failing.append((j, k2, leg, s2, e2, o2))
            try:
                os.unlink(p2)
            except OSError:
                pass
        slugs = set()
        for _j, _k, leg, _s, _e, _o in failing:
            slug = known_bug_slug(leg.tags)
            if slug:
                slugs.add(slug)
            else:
                slugs = None
                break
        if failing and slugs:
            kind = "KNOWN(" + ",".join(sorted(slugs)) + ")"
        detail = (failing, src, expected, out)
    try:
        os.unlink(path)
    except OSError:
        pass
    return kind, detail


# ---------------------------------------------------------------------------
# Self-test: prove the classifier sees each failure class.
# ---------------------------------------------------------------------------

SELF_TESTS = [
    ("clean pass",
     '#lang saffron\n(defn f [x] (+ x 1))\n(defn main [] : int (println (f 4.25)) 0)\n',
     "5.25\n", "ok"),
    ("wrong output detected",
     '#lang saffron\n(defn f [x] (+ x 1))\n(defn main [] : int (println (f 4.25)) 0)\n',
     "6.25\n", "BUG_wrong_output"),
    ("compiled crash detected",
     '#lang saffron\n(defn boom [] : int\n'
     '  ```c\n  volatile int64_t *p = 0;\n  *p = 1;\n  return 0;\n  ```)\n'
     '(defn main [] : int (println (boom)) 0)\n',
     "0\n", "BUG_crash"),
    ("type error rejected",
     '#lang saffron\n(defn f [x : int] : int "nope")\n(defn main [] : int (println (f 1)) 0)\n',
     "?\n", "GEN_REJECT"),
    # A runtime panic on the compiled arm is a crash, not "other": the
    # dynamic operator layer panics with exit 134 on a cstr operand.
    ("dynamic-op panic detected",
     '#lang saffron\n(defn f [x] (+ x 1))\n(defn main [] : int (println (f "s")) 0)\n',
     "?\n", "BUG_crash"),
]


def self_test(tur, workdir):
    ok = True
    for i, (label, src, expected, want) in enumerate(SELF_TESTS):
        path = os.path.join(workdir, "selftest%d.tur" % i)
        out = run_case(tur, path, src)
        got = classify(out, expected)
        status = "ok " if got == want else "FAIL"
        if got != want:
            ok = False
        print("  %s %-28s want=%-16s got=%s" % (status, label, want, got))
    # The interpreter arm: run the clean program's source against a wrong
    # prediction with the compiled arm forced clean, so an IBUG_ verdict is
    # exercised without needing an actual divergence in the compiler.
    o = Outcome("clean", "5.25\n", "", "clean", "6.25\n", "")
    got = classify(o, "5.25\n")
    want = "IBUG_wrong_output"
    print("  %s %-28s want=%-16s got=%s"
          % ("ok " if got == want else "FAIL", "interp wrong-output path", want, got))
    ok = ok and got == want
    o = Outcome("clean", "5.25\n", "", "abort", "", "panic")
    got = classify(o, "5.25\n")
    want = "IBUG_abort"
    print("  %s %-28s want=%-16s got=%s"
          % ("ok " if got == want else "FAIL", "interp abort path", want, got))
    ok = ok and got == want
    return ok


# One pinned minimal repro per open finding.  Each must FIRE (any non-ok
# classification on either arm) on an unfixed build.
KNOWN_PROBES = [
    ("H5  Sym inside an any: type-of / =",
     '#lang saffron\n(defn k [x] (type-of x))\n(defn s [a b] (= a b))\n'
     '(defn main [] : int (println (k :kw)) (println (s :a :a)) 0)\n', "Sym\ntrue\n"),
    ("H7  seam into a fn-typed parameter",
     '#lang saffron\n(defn tfn [f : (fn [int] int)] : int (f 1))\n(defn id [x] x)\n'
     '(defn main [] : int (println (tfn (id (fn [x : int] : int (+ x 1))))) 0)\n', "2\n"),
    ("H8  typed defn held in an any is not callable",
     '#lang saffron\n(defn app [f x] (f x))\n(defn inc [n : int] : int (+ n 1))\n'
     '(defn main [] : int (println (app inc 41)) 0)\n', "42\n"),
    ("H9  any-held map into map-get",
     '#lang saffron\n(defn lookup [m k : Sym] (map-get m k))\n'
     '(defn main [] : int (println (lookup #map{:a 1} :a)) 0)\n', "1\n"),
    ("M7  dynamic .head/.tail read on an any-held Cons",
     '#lang saffron\n(defn hd [l] (.head l))\n'
     '(defn main [] : int (println (hd (list 7.25 1))) 0)\n', "7.25\n"),
    ("M10 macro-expanded any (map-get) gets no seam into a typed param",
     '#lang saffron\n(defn s [v : int] : int v)\n'
     '(defn main [] : int (println (s (map-get #map{:k 7 :o 1} :k))) 0)\n', "7\n"),
    ("L   parametric ctor under a typed (W any) expectation builds (W int)",
     '#lang saffron\n(defdata W [a] (Wrap a))\n(defn s [v : (W any)] : (W any) v)\n'
     '(defn t [x] x)\n(defn main [] : int (println (match (t (s (Wrap 7))) (Wrap v) v)) 0)\n',
     "7\n"),
]


def known_probes(tur, workdir):
    print("known-probe status (open findings the generator avoids by default):")
    any_fixed = False
    for i, (label, src, expected) in enumerate(KNOWN_PROBES):
        path = os.path.join(workdir, "known%d.tur" % i)
        out = run_case(tur, path, src)
        kind = classify(out, expected)
        fired = kind != "ok"
        if not fired:
            any_fixed = True
        print("  %-62s %s" % (label, "fires (%s)" % kind if fired
                              else "FIXED -- retire its KNOWN row"))
    return any_fixed


# ---------------------------------------------------------------------------



def _progress(i, n, kind, extra=""):
    """Report a case the moment it finishes.  Every non-ok verdict prints its
    own line; ok cases print a heartbeat every 25 so a long session is
    visibly alive under a pipe or ctest -V.  Flushed, because stdout is
    block-buffered under a pipe and nothing would show until exit."""
    if kind != "ok":
        print("  case %6d/%d  %s%s" % (i, n, kind, extra), flush=True)
    elif (i + 1) % 25 == 0 or i + 1 == n:
        print("  %6d/%d done" % (i + 1, n), flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200, help="cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--legs", type=int, default=3, help="max legs per program")
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None)
    ap.add_argument("--emit-known", action="store_true",
                    help="also generate shapes matching open findings "
                         "(classified KNOWN, never fail the run)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--known-probes", action="store_true")
    args = ap.parse_args()

    tur = os.path.abspath(args.tur)
    if not os.path.exists(tur):
        print("saffron_fuzz_src: no compiler at %s" % tur, file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="saffron-fuzz-work-")
    try:
        if args.self_test:
            print("saffron_fuzz_src: self-test (%s)" % tur)
            ok = self_test(tur, workdir)
            print("saffron_fuzz_src: self-test %s" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
        if args.known_probes:
            known_probes(tur, workdir)
            return 0

        save_dir = args.save_dir or tempfile.mkdtemp(prefix="saffron-fuzz-src-")
        os.makedirs(save_dir, exist_ok=True)

        counts, findings = {}, []

        def job(i):
            return one_case(tur, workdir, i, args.seed * 1000003 + i,
                            args.legs, args.emit_known)

        print("saffron_fuzz_src: %d cases, seed %d, max %d legs, %d job(s)%s"
              % (args.n, args.seed, args.legs, args.jobs,
                 ", emit-known" if args.emit_known else ""))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(job, i): i for i in range(args.n)}
            done = 0
            for fut in as_completed(futs):
                i = futs[fut]
                kind, detail = fut.result()
                counts[kind] = counts.get(kind, 0) + 1
                if detail:
                    findings.append((kind, i, detail))
                extra = ""
                if detail and detail[0]:
                    extra = "  tags=" + " | ".join(
                        ",".join(sorted(leg.tags)) for _j, _k, leg, *_ in detail[0])
                _progress(done, args.n, kind, extra)
                done += 1
        findings.sort(key=lambda t: t[1])

        for kind, i, (failing, src, expected, out) in findings:
            safe = kind.replace("(", "_").replace(")", "").replace(",", "+")
            d = os.path.join(save_dir, "%s-%06d" % (safe, i))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "input.tur"), "w") as f:
                f.write(src)
            with open(os.path.join(d, "README"), "w") as f:
                f.write("classification: %s\nseed base: %d  case: %d\n"
                        "expected stdout:\n%s\ncompiled (%s):\n%r\nstderr tail:\n%s\n"
                        "interp (%s):\n%r\nstderr tail:\n%s\n"
                        % (kind, args.seed, i, expected, out.kind, out.stdout,
                           _errs(out.stderr), out.ikind, out.istdout,
                           _errs(out.istderr)))
                if failing:
                    f.write("\nbisected failing legs:\n")
                else:
                    f.write("\nINTERACTION: no single leg fails alone\n")
                for j, k2, leg, s2, e2, o2 in failing:
                    f.write("  leg %d: %s  tags=%s\n"
                            % (j, k2, ",".join(sorted(leg.tags))))
            for j, k2, leg, s2, e2, o2 in failing:
                with open(os.path.join(d, "leg%d.tur" % j), "w") as f:
                    # `#lang` has to be the first line of the file.
                    first, rest = s2.split("\n", 1)
                    f.write(first + "\n")
                    f.write(";; %s  tags=%s\n;; expected:\n"
                            % (k2, ",".join(sorted(leg.tags))))
                    for ln in e2.splitlines():
                        f.write(";;   %s\n" % ln)
                    f.write(rest)

        print("\nsaffron_fuzz_src: %d cases" % args.n)
        for k in sorted(counts):
            print("  %-28s : %d" % (k, counts[k]))

        n_bugs = sum(v for k, v in counts.items()
                     if k.startswith(("BUG", "IBUG")))
        n_rej = counts.get("GEN_REJECT", 0)
        n_known = sum(v for k, v in counts.items() if k.startswith("KNOWN"))
        print("\n  BUG/IBUG classes (fail)     : %d" % n_bugs)
        print("  generator rejects (report)  : %d" % n_rej)
        print("  known open findings (report): %d" % n_known)
        if findings:
            print("\n  saved to %s" % save_dir)
        elif not args.save_dir:
            shutil.rmtree(save_dir, ignore_errors=True)
        return 1 if n_bugs else 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
