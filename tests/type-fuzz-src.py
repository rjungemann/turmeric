#!/usr/bin/env python3
"""tests/type-fuzz-src.py -- SOURCE-LEVEL fuzzer for typed-boundary plumbing.

Why this exists
---------------
`tests/refine-fuzz-src.py` proved the pattern: generate whole programs, run
them through the real pipeline, compare against a property the design forbids
violating.  This harness applies the same pattern to a different bug family --
the one behind `result-monad-bind-typed-boundary-miscompiles`,
`poly-result-hof-capturing-closure-sigbus`, `generic-closure-return-type-app`,
and the No-Lazy-`:int` rule in CLAUDE.md.

Every one of those bugs has the same anatomy: a value's REAL type (a by-value
struct, a closure with an environment, a type application) meets the int64
carrier erasure at some boundary -- a function return, a `(fn ...)` parameter,
a typeclass method result, a Vec element slot -- and the two sides disagree
about the representation.  The full representation/boundary inventory this
harness walks is documented in docs/guides/value-representations-guide.md;
keep the two in sync.  The checker accepts, and then either cc rejects the
emitted C (`invalid initializer`), the linker misses a symbol, or the binary
reads a struct out of a pointer and dies.  Hand-written fixtures only cover
the combinations someone thought of; the reports above are all combinations
nobody did.

The property that matters
-------------------------
Every generated program is CORRECT BY CONSTRUCTION: the generator tracks the
concrete value it routes through the plumbing and knows the exact stdout the
program must produce.  So the oracle is total:

    tur check accepts  ==>  the C compiles, links, runs cleanly,
                            and prints exactly the predicted output

Four BUG classes fall out, each a checker/codegen disagreement:

  * BUG_invalid_c      -- `tur check` passed, cc rejected the emitted C
  * BUG_link           -- cc compiled, the link failed (undefined symbol)
  * BUG_crash          -- the binary died (SIGSEGV/SIGBUS/SIGABRT)
  * BUG_wrong_output   -- ran clean, printed the wrong values

One report-only class:

  * GEN_REJECT         -- `tur check` rejected a program the generator claims
    is legal.  Either the generator is wrong (fix it here) or the checker is
    (that is `generic-closure-return-type-app` Defect A's shape).  Saved for
    triage, never failed on, because the generator's legality claim is not
    itself machine-checked.

What it generates
-----------------
A program is 1-4 independent LEGS.  Each leg routes a known scalar (int,
float, bool, cstr) through a randomly chosen WRAPPER (bare, by-value struct,
:heap struct, ADT, (Option T), (Result T int), (Vec T), (Option Box),
(Result Box int), a capturing thunk) and then across 1-3 randomly chosen
BOUNDARY CROSSINGS while wrapped:

    pass-through defn / let-in-defn / if-in-defn / recursive defn
    let binding / ascription `(:: e T)` / generic identity `[A] x:A -> A`
    ^fat HOF (capturing closure in)  / thin HOF (carrier-safe types only)
    closure RETURN (defn returning `(fn [] T)` that captures its argument)
    typeclass method dispatch (bare and dotted)

then unwraps and prints.  Floats are printed as `(= e <lit>)` against the
generator's own literal (all float values used are exact in binary and only
+,-,* touch ints), so float formatting never enters the oracle.

Known open bugs
---------------
Shapes that reproduce ALREADY-FILED open reports are excluded from generation
by default -- a fuzz run's value is what it finds that is NOT yet on file.
`known_bug_slug()` is the single place that knowledge lives; `--emit-known`
re-enables those shapes (each finding is then classified KNOWN_<class> and
never fails the run) -- useful to watch a fix land.  `--known-probes` runs a
pinned minimal repro per open report and prints fires/fixed, so the avoid
list cannot silently rot after the fixes land: a `fixed` row means the entry
should be retired from `known_bug_slug` and its shape returned to the
default pool.

Attribution
-----------
A failing program is auto-bisected: each leg is rerun in isolation and the
failing leg(s) are saved with their shape tags, so a finding reads as
"wrapper=res route=fat_hof,gid scalar=float" rather than a 120-line blob.  If
no leg fails alone, the whole program is saved as an INTERACTION finding --
those are the interesting ones.

Proving the fuzzer can fail
---------------------------
`--self-test` feeds the classifier four fixed programs: a clean pass, a
deliberate wrong-output, a guaranteed crash (null write in inline C), and a
type error.  All four classifications are asserted.  Beyond plumbing, the
harness demonstrably detects every open report in the known table: run
`--known-probes` and watch all of them fire on an unfixed build.

Usage
-----
    python3 tests/type-fuzz-src.py [--n 200] [--seed 1] [--jobs N]
                                   [--tur ./build/tur] [--save-dir DIR]
                                   [--legs 1..4] [--emit-known]
                                   [--self-test] [--known-probes]

Exit status is 1 if any BUG class fired (GEN_REJECT and KNOWN_* never fail
the run), 0 otherwise.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TIMEOUT = 90

# ---------------------------------------------------------------------------
# Known open reports -- the single place this knowledge lives.
#
# Each entry: (slug, predicate over a leg's tag set).  A leg whose tags match
# is (a) not generated by default and (b) classified KNOWN_* when --emit-known
# turns it back on.  When a report is fixed and archived, delete its row here
# and the shape returns to the default pool.
# ---------------------------------------------------------------------------

BYVALUE_WRAPPERS = {"box", "adt", "opt", "res", "opt_box", "res_box"}


CROSSING_TAGS = {"through", "deep", "let", "ascribe", "gid", "fat_hof",
                 "thin_hof", "class_thru", "tyvar_run"}


def known_bug_slug(tags):
    """Return the docs/reported slug a leg shape reproduces, or None."""
    # (Retired 2026-07-30 by fn-value-fat-normalization stage 2: fn-typed
    # VALUES survive pass-through returns, ascribe-around-let, and nested
    # fat HOFs now -- thunk legs are back in the full crossing pool.)
    # Residual seams stage 2's first full-pool sessions surfaced: a thunk
    # value through a pass-through DEFN (through/deep) hits the carrier<->fat
    # alias/unification gaps.  let/ascribe/gid/HOF crossings are fine.
    if "thunk" in tags and tags & {"through", "deep"}:
        return "fn-value-carrier-fat-seam-residuals"
    # (Retired 2026-07-30 by fn-value-fat-normalization stage 1: thin fn
    # params with CONCRETE non-carrier-safe signatures -- by-value and heap
    # results/args -- are fat-normalized now, so those thin_hof shapes are
    # back in the default pool.  The tyvar-result rule below remains: tyvar
    # signatures are excluded from the narrowed stage-1 claim.)
    # A capturing closure through a tyvar-result HOF ((fn [] R) : R) crashes
    # for every wrapper -- same report, widest trigger.
    if "tyvar_run" in tags:
        return "poly-result-hof-capturing-closure-sigbus"
    # (result-monad-bind-typed-boundary-miscompiles: RESOLVED 2026-07-31 by
    # consolidation increment 2 -- continuation-wrapper ABI paired with the
    # selected entry point + ascription-aware carrier-type recovery -- and
    # archived; bind legs are in the DEFAULT generation rotation now.)
    # (vec-byvalue-struct-element-invalid-c: RESOLVED 2026-07-31 by
    # consolidation increment 3 -- any-width by-value products are heap-boxed
    # into container slots (push-side escaping bridge, read-side deref-unbox,
    # ownership probes in lockstep via type_is_boxed_container_elem) -- and
    # archived; vec_box_byvalue wrappers are in the DEFAULT pool now.)
    # (class-method-result-into-generic-invalid-c: RESOLVED 2026-07-31 by
    # consolidation increment 2 -- the carrier-producer classifier now knows
    # M7 by-value instance results -- and archived; rows retired.)
    return None


# Pinned minimal repros, one per open report above, used by --known-probes to
# keep the avoid list honest: when one prints `fixed`, retire its
# known_bug_slug row.
KNOWN_PROBES = [
    ("poly-result-hof-capturing-closure-sigbus (tyvar result)",
     "(defn run [R] [body : (fn [] R)] : R (body))\n"
     "(defn main [] : int\n"
     "  (let [k 7] (println (run (fn [] : int (+ k 1)))))\n  0)\n"),
    ("poly-result-hof-capturing-closure-sigbus (by-value struct result)",
     "(defstruct FzB [a : int])\n"
     "(defn call [f : (fn [] FzB)] : FzB (f))\n"
     "(defn main [] : int\n"
     "  (let [k 7] (println (.a (call (fn [] (FzB k))))))\n  0)\n"),
    # (result-monad-bind-typed-boundary-miscompiles: RESOLVED 2026-07-31,
    # archived; probe retired -- pinned by
    # tests/fixtures/result-monad-bind-typed-boundary/.)
    # Faithful to the report: it takes the stdlib Cons (a defstruct).  The
    # same shape over a local parametric defdata checks AND runs clean, so
    # the trigger is narrower than "generic + type-app + closure return".
    ("generic-closure-return-type-app (Defect A: checker reject)",
     "(defn pure [A] [x : A] : (fn [] (Cons A))\n"
     "  (fn [] (tcons x (tnil))))\n"
     "(defn use [A] [xs : (Cons A)] : int 0)\n"
     "(defn main [] : int (use ((pure 1))))\n"),
    # (vec-byvalue-struct-element-invalid-c: RESOLVED 2026-07-31, archived;
    # probe retired -- pinned by tests/fixtures/vec-byvalue-struct-element/
    # and tests/fixtures/map-narrow-struct-value/.)
    # (fn-typed-value-return-ascribe-miscompiles: RESOLVED 2026-07-30 by
    # fat-normalization stage 2 and archived; its matrix -- broken rows
    # included -- is pinned by tests/fixtures/fn-value-matrix-ok-rows/, so
    # the probes are retired rather than kept as permanent FIXED rows.)
    # (class-method-result-into-generic-invalid-c: RESOLVED 2026-07-31,
    # archived; probe retired -- pinned by
    # tests/fixtures/class-method-result-into-generic/.)
    # (fn-payload-in-container-undeclared-temp: RESOLVED 2026-07-31 by
    # fat-normalization stage 2 -- the parametric-defdata + FLOAT variant
    # verified by hand -- and archived; probe retired.)
    ("fn-value-carrier-fat-seam-residuals (let-aliased carrier param tail)",
     "(defn thru2 [v : (fn [] float)] : (fn [] float) (let [w v] w))\n"
     "(defn main [] : int\n"
     "  (let [k 1.25]\n"
     "    (println (= ((thru2 (fn [] k))) 1.25)))\n  0)\n"),
]


# ---------------------------------------------------------------------------
# Leg generation
# ---------------------------------------------------------------------------

FLOAT_LITS = ["0.5", "1.25", "2.75", "3.25", "7.1", "-1.5", "-4.25", "6.5",
              "7.25", "10.5"]
CSTR_POOL = ["alpha", "brave-7", "c3", "delta-x", "e", "fzz-99", "gg-hh"]


class Leg:
    """One independent value route: defns + main-body form + expected lines."""

    def __init__(self):
        self.defs = []        # top-level defn/defstruct/... source strings
        self.body = []        # forms for main (strings)
        self.expected = []    # exact stdout lines
        self.tags = set()     # shape tags for attribution / known matching


class Gen:
    def __init__(self, rng, idx, emit_known):
        self.rng = rng
        self.i = idx              # leg index, used to prefix every name
        self.emit_known = emit_known
        self.n_names = 0

    def name(self, stem):
        self.n_names += 1
        return "l%d%s%d" % (self.i, stem, self.n_names)

    # -- scalars --------------------------------------------------------------

    def pick_scalar(self):
        t = self.rng.choice(["int", "int", "int", "float", "bool", "cstr"])
        if t == "int":
            return t, self.rng.randint(-20, 20)
        if t == "float":
            return t, self.rng.choice(FLOAT_LITS)
        if t == "bool":
            return t, self.rng.choice([True, False])
        return t, "%s-%d" % (self.rng.choice(CSTR_POOL), self.rng.randint(0, 99))

    def lit(self, ty, v):
        if ty == "int":
            return str(v)
        if ty == "float":
            return v
        if ty == "bool":
            return "true" if v else "false"
        return '"%s"' % v

    # -- wrappers -------------------------------------------------------------
    #
    # Each returns (typename, wrap_fn, unwrap_fn, tags) where wrap_fn/unwrap_fn
    # map an expression string to an expression string, emitting helper defns
    # into `leg`.  Wrapping and unwrapping are themselves defns, so every
    # wrapper already crosses two function boundaries typed.

    def w_none(self, leg, ty):
        return ty, (lambda e: e), (lambda e: e), {"bare"}

    def w_box(self, leg, ty, heap=False):
        tn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s%s [a : %s])"
                        % (tn, " :heap" if heap else "", ty))
        w, u = self.name("bw"), self.name("bu")
        leg.defs.append("(defn %s [x : %s] : %s (%s x))" % (w, ty, tn, tn))
        # Extraction: direct field read, or through a typeclass instance --
        # the method result is where the erasure historically leaks.
        if self.rng.random() < 0.35:
            cls, meth = "FzC%d%d" % (self.i, self.n_names), self.name("m")
            leg.defs.append("(defclass %s [a] (%s [self : a] : %s))"
                            % (cls, meth, ty))
            leg.defs.append("(definstance %s [%s] (%s [self : %s] : %s (.a self)))"
                            % (cls, tn, meth, tn, ty))
            dot = "." if self.rng.random() < 0.5 else ""
            leg.defs.append("(defn %s [b : %s] : %s (%s%s b))"
                            % (u, tn, ty, dot, meth))
            tags = {"box_heap" if heap else "box", "class_extract"}
        else:
            leg.defs.append("(defn %s [b : %s] : %s (.a b))" % (u, tn, ty))
            tags = {"box_heap" if heap else "box"}
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), tags

    def w_adt(self, leg, ty):
        tn, ctor = "FzW%d%d" % (self.i, self.n_names), None
        self.n_names += 1
        ctor = tn + "c"
        leg.defs.append("(defdata %s (%s :%s))" % (tn, ctor, ty))
        w, u = self.name("aw"), self.name("au")
        leg.defs.append("(defn %s [x : %s] : %s (%s x))" % (w, ty, tn, ctor))
        leg.defs.append("(defn %s [v : %s] : %s (match v (%s x) x))"
                        % (u, tn, ty, ctor))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"adt"}

    def w_opt(self, leg, ty):
        tn = "(Option %s)" % ty
        w, u = self.name("ow"), self.name("ou")
        dflt = {"int": "-9999", "float": "-999.5", "bool": "false",
                "cstr": '"z-dflt"'}[ty]
        leg.defs.append("(defn %s [x : %s] : %s (some x))" % (w, ty, tn))
        leg.defs.append("(defn %s [o : %s] : %s (unwrap-or o %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"opt"}

    def w_res(self, leg, ty):
        tn = "(Result %s int)" % ty
        w, u = self.name("rw"), self.name("ru")
        dflt = {"int": "-9999", "float": "-999.5", "bool": "false",
                "cstr": '"z-dflt"'}[ty]
        leg.defs.append("(defn %s [x : %s] : %s (ok x))" % (w, ty, tn))
        leg.defs.append("(defn %s [r : %s] : %s (if (ok? r) (ok-val r) %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"res"}

    def w_vec(self, leg, ty):
        tn = "(Vec %s)" % ty
        w, u = self.name("vw"), self.name("vu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v x)\n    v))" % (w, ty, tn, tn))
        get = "(vec-get v 0)"
        if ty != "int":
            get = "(:: %s :%s)" % (get, ty)
        leg.defs.append("(defn %s [v : %s] : %s %s)" % (u, tn, ty, get))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"vec"}

    def w_vec_box_heap(self, leg, ty):
        # Vec of :heap struct elements works; the by-value variant is the
        # known invalid-C shape (see known_bug_slug).
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s :heap [a : %s])" % (bn, ty))
        tn = "(Vec %s)" % bn
        w, u = self.name("hw"), self.name("hu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v (%s x))\n    v))" % (w, ty, tn, tn, bn))
        leg.defs.append(
            "(defn %s [v : %s] : %s\n"
            "  (.a (:: (vec-get v 0) %s)))" % (u, tn, ty, bn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"vec_heap_struct"}

    def w_vec_box_byvalue(self, leg, ty):
        # In the DEFAULT pool since increment 3 (2026-07-31): any-width
        # by-value struct elements ride container slots heap-boxed now.
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Vec %s)" % bn
        w, u = self.name("yw"), self.name("yu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v (%s x))\n    v))" % (w, ty, tn, tn, bn))
        leg.defs.append(
            "(defn %s [v : %s] : %s\n"
            "  (.a (:: (vec-get v 0) %s)))" % (u, tn, ty, bn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"vec_byvalue_struct"}

    def w_opt_box(self, leg, ty):
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Option %s)" % bn
        w, u = self.name("pw"), self.name("pu")
        dflt = {"int": "-9999", "float": "-999.5", "bool": "false",
                "cstr": '"z-dflt"'}[ty]
        leg.defs.append("(defn %s [x : %s] : %s (some (%s x)))" % (w, ty, tn, bn))
        leg.defs.append("(defn %s [o : %s] : %s (.a (unwrap-or o (%s %s))))"
                        % (u, tn, ty, bn, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"opt_box"}

    def w_res_box(self, leg, ty):
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Result %s int)" % bn
        w, u = self.name("qw"), self.name("qu")
        dflt = {"int": "-9999", "float": "-999.5", "bool": "false",
                "cstr": '"z-dflt"'}[ty]
        leg.defs.append("(defn %s [x : %s] : %s (ok (%s x)))" % (w, ty, tn, bn))
        leg.defs.append("(defn %s [r : %s] : %s\n"
                        "  (if (ok? r) (.a (ok-val r)) %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"res_box"}

    def w_thunk(self, leg, ty):
        # defn returning `(fn [] ty)` that closes over its argument: the
        # closure-RETURN boundary.
        tn = "(fn [] %s)" % ty
        w = self.name("tw")
        leg.defs.append("(defn %s [x : %s] : %s (fn [] x))" % (w, ty, tn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s)" % e), \
            {"thunk", "closure_ret"}

    WRAPPERS = ["none", "box", "box_heap", "adt", "opt", "res", "vec",
                "vec_box_heap", "vec_box_byvalue", "opt_box", "res_box",
                "thunk"]

    def pick_wrapper(self, leg, ty):
        pool = list(self.WRAPPERS)
        which = self.rng.choice(pool)
        if which == "none":
            return self.w_none(leg, ty)
        if which == "box":
            return self.w_box(leg, ty, heap=False)
        if which == "box_heap":
            return self.w_box(leg, ty, heap=True)
        if which == "adt":
            return self.w_adt(leg, ty)
        if which == "opt":
            return self.w_opt(leg, ty)
        if which == "res":
            return self.w_res(leg, ty)
        if which == "vec":
            return self.w_vec(leg, ty)
        if which == "vec_box_heap":
            return self.w_vec_box_heap(leg, ty)
        if which == "vec_box_byvalue":
            return self.w_vec_box_byvalue(leg, ty)
        if which == "opt_box":
            return self.w_opt_box(leg, ty)
        return self.w_thunk(leg, ty) if which == "thunk" else self.w_res_box(leg, ty)

    # -- boundary crossings ----------------------------------------------------
    #
    # Each takes (leg, typename, expr) and returns (expr', tag).  The crossing
    # happens while the value is WRAPPED -- that is the whole point.

    def x_through(self, leg, tn, e):
        f = self.name("f")
        style = self.rng.random()
        if style < 0.4:
            leg.defs.append("(defn %s [v : %s] : %s v)" % (f, tn, tn))
        elif style < 0.65:
            leg.defs.append("(defn %s [v : %s] : %s (let [w v] w))" % (f, tn, tn))
        elif style < 0.85:
            c = self.rng.choice(["(= 1 1)", "(> 2 1)", "(not= 3 4)"])
            leg.defs.append("(defn %s [v : %s] : %s (if %s v v))" % (f, tn, tn, c))
        else:
            leg.defs.append("(defn %s [n : int v : %s] : %s\n"
                            "  (if (= n 0) v (%s (- n 1) v)))" % (f, tn, tn, f))
            return "(%s %d %s)" % (f, self.rng.randint(1, 3), e), "deep"
        return "(%s %s)" % (f, e), "through"

    def x_let(self, leg, tn, e):
        v = self.name("v")
        return "(let [%s %s] %s)" % (v, e, v), "let"

    def x_ascribe(self, leg, tn, e):
        return "(:: %s %s)" % (e, tn), "ascribe"

    def x_gid(self, leg, tn, e):
        f = self.name("g")
        leg.defs.append("(defn %s [A] [x : A] : A x)" % f)
        n = 2 if self.rng.random() < 0.3 else 1
        for _ in range(n):
            e = "(%s %s)" % (f, e)
        return e, "gid"

    def x_fat_hof(self, leg, tn, e):
        f = self.name("h")
        leg.defs.append("(defn %s [^fat f : (fn [] %s)] : %s (f))" % (f, tn, tn))
        v = self.name("c")
        # let-bind first so the lambda genuinely captures a local.
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "fat_hof"

    def x_thin_hof(self, leg, tn, e):
        f = self.name("t")
        leg.defs.append("(defn %s [f : (fn [] %s)] : %s (f))" % (f, tn, tn))
        v = self.name("c")
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "thin_hof"

    def x_tyvar_run(self, leg, tn, e):
        # KNOWN shape (crashes with a capturing closure); --emit-known only.
        f = self.name("r")
        leg.defs.append("(defn %s [R] [body : (fn [] R)] : R (body))" % f)
        v = self.name("c")
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "tyvar_run"

    def x_class_thru(self, leg, tn, e):
        # Typeclass pass-through: instance heads must be plain names, so this
        # crossing only applies to scalar and struct/ADT wrappers.
        cls, meth = "FzT%d%d" % (self.i, self.n_names), self.name("p")
        leg.defs.append("(defclass %s [a] (%s [self : a] : a))" % (cls, meth))
        leg.defs.append("(definstance %s [%s] (%s [self : %s] : %s self))"
                        % (cls, tn, meth, tn, tn))
        dot = "." if self.rng.random() < 0.5 else ""
        return "(%s%s %s)" % (dot, meth, e), "class_thru"

    def crossings_for(self, tn, tags):
        # Fn-typed VALUES are fat-normalized across returns/let/ascribe and
        # HOF hops as of fn-value-fat-normalization stage 2 (2026-07-30) --
        # thunk legs take the full crossing pool minus the class/gid pair
        # (instance heads need plain names; gid over a thunk is fine and
        # included).
        if "thunk" in tags:
            return [self.x_through, self.x_let, self.x_ascribe, self.x_gid,
                    self.x_fat_hof, self.x_thin_hof]
        xs = [self.x_through, self.x_let, self.x_ascribe, self.x_gid,
              self.x_fat_hof, self.x_thin_hof]
        # Thin HOF over every wrapper: scalars ride the poly carrier;
        # concrete by-value/heap signatures are fat-normalized as of
        # fn-value-fat-normalization stage 1 (2026-07-30).
        # Instance heads: plain type names only.
        if not tn.startswith("("):
            xs.append(self.x_class_thru)
        if self.emit_known:
            xs.append(self.x_tyvar_run)
        return xs

    # -- int mutation steps (bare int legs get arithmetic through defns) -------

    def int_mut(self, leg, e, v):
        f = self.name("i")
        k = self.rng.randint(1, 9)
        op = self.rng.choice(["+", "-", "*"])
        if op == "*":
            k = self.rng.choice([2, 3])
        leg.defs.append("(defn %s [x : int] : int (%s x %s))" % (f, op, k))
        nv = {"+": v + k, "-": v - k, "*": v * k}[op]
        return "(%s %s)" % (f, e), nv

    # -- one leg ----------------------------------------------------------------

    def leg(self):
        leg = Leg()
        ty, val = self.pick_scalar()
        tn, wrap, unwrap, tags = self.pick_wrapper(leg, ty)
        leg.tags |= tags
        leg.tags.add("scalar_" + ty)

        e = self.lit(ty, val)
        if ty == "int" and "bare" in tags and self.rng.random() < 0.7:
            for _ in range(self.rng.randint(1, 2)):
                e, val = self.int_mut(leg, e, val)

        e = wrap(e)
        pool = self.crossings_for(tn, tags)
        if pool:
            for _ in range(self.rng.randint(1, 3)):
                x = self.rng.choice(pool)
                e, tag = x(leg, tn, e)
                leg.tags.add(tag)
        e = unwrap(e)

        if ty == "float":
            leg.body.append("(println (= %s %s))" % (e, self.lit(ty, val)))
            leg.expected.append("true")
        elif ty == "bool":
            leg.body.append("(println %s)" % e)
            leg.expected.append("true" if val else "false")
        elif ty == "cstr":
            leg.body.append("(println %s)" % e)
            leg.expected.append(val)
        else:
            leg.body.append("(println %s)" % e)
            leg.expected.append(str(val))
        return leg

    def leg_res_bind(self):
        """Monad bind over Result through a typed defn boundary.  Fixed by
        consolidation increment 2 (was result-monad-bind-typed-boundary,
        archived); generated in the default rotation since."""
        leg = Leg()
        f, g = self.name("bf"), self.name("bg")
        n = self.rng.randint(1, 9)
        leg.defs.append("(defn %s [n : int] : (Result int int)\n"
                        "  (if (= n 0) (err 7) (ok n)))" % f)
        leg.defs.append("(defn %s [n : int] : (Result int int)\n"
                        "  (bind (%s n) (fn [x] (ok (* x 2)))))" % (g, f))
        leg.body.append("(let [r (%s %d)]\n"
                        "    (println (if (ok? r) (ok-val r) -1)))" % (g, n))
        leg.expected.append(str(n * 2))
        leg.tags = {"res_bind", "scalar_int"}
        return leg


def gen_program(rng, n_legs, emit_known):
    legs = []
    for i in range(n_legs):
        g = Gen(rng, i, emit_known)
        # Bind legs run in the default rotation since the increment-2 fix.
        if rng.random() < 0.10:
            legs.append(g.leg_res_bind())
        else:
            legs.append(g.leg())
    return legs


def assemble(legs):
    defs, body, expected = [], [], []
    for leg in legs:
        defs += leg.defs
        body += leg.body
        expected += leg.expected
    src = "\n\n".join(defs)
    src += "\n\n(defn main [] : int\n  " + "\n  ".join(body) + "\n  0)\n"
    return src, "\n".join(expected) + "\n"


# ---------------------------------------------------------------------------
# Running one case
# ---------------------------------------------------------------------------

class Outcome:
    def __init__(self, kind, stdout="", stderr=""):
        self.kind = kind        # clean/crash/invalid_c/link/reject/timeout/other
        self.stdout = stdout
        self.stderr = stderr


def run_case(tur, path, src):
    with open(path, "w") as f:
        f.write(src)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") or "detect_leaks=0"
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
        return Outcome("clean", p.stdout, p.stderr)
    if p.returncode in (134, 138, 139) or p.returncode < 0:
        return Outcome("crash", p.stdout, p.stderr)
    blob = p.stderr + p.stdout
    if "cc invocation failed" in blob:
        if "undefined" in blob.lower() or "ld:" in blob:
            return Outcome("link", p.stdout, p.stderr)
        return Outcome("invalid_c", p.stdout, p.stderr)
    return Outcome("other", p.stdout, p.stderr)


BUG_OF = {"crash": "BUG_crash", "invalid_c": "BUG_invalid_c",
          "link": "BUG_link", "other": "BUG_toolchain_other"}


def classify(out, expected):
    if out.kind == "timeout":
        return "skip_timeout"
    if out.kind == "reject":
        return "GEN_REJECT"
    if out.kind == "clean":
        return "ok" if out.stdout == expected else "BUG_wrong_output"
    return BUG_OF[out.kind]


def one_case(tur, workdir, idx, seed, max_legs, emit_known):
    rng = random.Random(seed)
    legs = gen_program(rng, rng.randint(1, max_legs), emit_known)
    src, expected = assemble(legs)
    path = os.path.join(workdir, "c%06d.tur" % idx)
    out = run_case(tur, path, src)
    kind = classify(out, expected)

    detail = None
    if kind.startswith("BUG") or kind == "GEN_REJECT":
        # Bisect: which leg(s) fail alone?
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
        # KNOWN downgrade: every failing leg matches an open report.
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
     '(defn f [x : int] : int (+ x 1))\n(defn main [] : int (println (f 4)) 0)\n',
     "5\n", "ok"),
    ("wrong output detected",
     '(defn f [x : int] : int (+ x 1))\n(defn main [] : int (println (f 4)) 0)\n',
     "6\n", "BUG_wrong_output"),
    ("crash detected",
     '(defn boom [] : int\n'
     '  ```c\n  volatile int64_t *p = 0;\n  *p = 1;\n  return 0;\n  ```)\n'
     '(defn main [] : int (println (boom)) 0)\n',
     "0\n", "BUG_crash"),
    ("type error rejected",
     '(defn f [x : int] : int "nope")\n(defn main [] : int (println (f 1)) 0)\n',
     "?\n", "GEN_REJECT"),
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
    return ok


def known_probes(tur, workdir):
    print("known-probe status (open reports the generator avoids by default):")
    any_fixed = False
    for i, (label, src) in enumerate(KNOWN_PROBES):
        path = os.path.join(workdir, "known%d.tur" % i)
        out = run_case(tur, path, src)
        fired = out.kind in ("crash", "invalid_c", "link", "reject", "other")
        if not fired:
            any_fixed = True
        print("  %-62s %s" % (label, "fires (%s)" % out.kind if fired
                              else "FIXED -- retire its known_bug_slug row"))
    return any_fixed


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200, help="cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--legs", type=int, default=3, help="max legs per program")
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None)
    ap.add_argument("--emit-known", action="store_true",
                    help="also generate shapes matching open reports "
                         "(classified KNOWN, never fail the run)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--known-probes", action="store_true")
    args = ap.parse_args()

    tur = os.path.abspath(args.tur)
    if not os.path.exists(tur):
        print("type_fuzz_src: no compiler at %s" % tur, file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="type-fuzz-work-")
    try:
        if args.self_test:
            print("type_fuzz_src: self-test (%s)" % tur)
            ok = self_test(tur, workdir)
            print("type_fuzz_src: self-test %s" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
        if args.known_probes:
            known_probes(tur, workdir)
            return 0

        save_dir = args.save_dir or tempfile.mkdtemp(prefix="type-fuzz-src-")
        os.makedirs(save_dir, exist_ok=True)

        counts, findings = {}, []

        def job(i):
            return one_case(tur, workdir, i, args.seed * 1000003 + i,
                            args.legs, args.emit_known)

        print("type_fuzz_src: %d cases, seed %d, max %d legs, %d job(s)%s"
              % (args.n, args.seed, args.legs, args.jobs,
                 ", emit-known" if args.emit_known else ""))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for i, (kind, detail) in enumerate(pool.map(job, range(args.n))):
                counts[kind] = counts.get(kind, 0) + 1
                if detail:
                    findings.append((kind, i, detail))

        for kind, i, (failing, src, expected, out) in findings:
            safe = kind.replace("(", "_").replace(")", "").replace(",", "+")
            d = os.path.join(save_dir, "%s-%06d" % (safe, i))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "input.tur"), "w") as f:
                f.write(src)
            with open(os.path.join(d, "README"), "w") as f:
                f.write("classification: %s\nseed base: %d  case: %d\n"
                        "expected stdout:\n%s\ngot (%s):\n%r\nstderr tail:\n%s\n"
                        % (kind, args.seed, i, expected, out.kind, out.stdout,
                           out.stderr[-2000:]))
                if failing:
                    f.write("\nbisected failing legs:\n")
                else:
                    f.write("\nINTERACTION: no single leg fails alone\n")
                for j, k2, leg, s2, e2, o2 in failing:
                    f.write("  leg %d: %s  tags=%s\n"
                            % (j, k2, ",".join(sorted(leg.tags))))
            for j, k2, leg, s2, e2, o2 in failing:
                with open(os.path.join(d, "leg%d.tur" % j), "w") as f:
                    f.write(";; %s  tags=%s\n;; expected:\n"
                            % (k2, ",".join(sorted(leg.tags))))
                    for ln in e2.splitlines():
                        f.write(";;   %s\n" % ln)
                    f.write(s2)

        print("\ntype_fuzz_src: %d cases" % args.n)
        for k in sorted(counts):
            print("  %-28s : %d" % (k, counts[k]))

        n_bugs = sum(v for k, v in counts.items() if k.startswith("BUG"))
        n_rej = counts.get("GEN_REJECT", 0)
        n_known = sum(v for k, v in counts.items() if k.startswith("KNOWN"))
        print("\n  BUG classes (fail)          : %d" % n_bugs)
        print("  generator rejects (report)  : %d" % n_rej)
        print("  known open reports (report) : %d" % n_known)
        if findings:
            print("\n  saved to %s" % save_dir)
        elif not args.save_dir:
            shutil.rmtree(save_dir, ignore_errors=True)
        return 1 if n_bugs else 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
