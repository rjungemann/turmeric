#!/usr/bin/env python3
"""tests/refine-fuzz-src.py -- SOURCE-LEVEL differential fuzzer for refinement
types.

Why this exists
---------------
`tests/unit/refine_fuzz.c` generates normalized VCs directly and compares the
in-house solver's verdicts against Z3.  It is a good solver fuzzer and a
useless compiler fuzzer: it starts *below* the encoder, so every bug in the
translation from Turmeric source into a VC is invisible to it.  Both soundness
bugs found in the refinement work so far lived exactly there, and ~17,300
randomly generated VCs across six seeds were clean the whole time.

This harness starts at the top instead.  It generates whole Turmeric programs,
compiles and runs each one TWICE -- once with the `refined` experiment off and
once with it on -- and compares what actually happened.

The property that matters
-------------------------
With the gate OFF, every refinement is a runtime contract check.  A program
that violates its own refinement aborts.  With the gate ON, a discharged
obligation lets that check be elided.  So:

    gate-off aborted on a contract  ==>  gate-on must NOT run to completion

Any case where gate-off aborts and gate-on exits 0 is a MISCOMPILE: turning the
experiment on changed a program that was caught into one that silently returns
a value violating its own declared refinement.  That is the single thing the
design forbids, and it is precisely what both known bugs did.

Three weaker properties are checked alongside it:

  * both gates ran clean  ->  stdout must be identical (elision must not change
    observable behaviour)
  * gate-off ran clean    ->  gate-on must not introduce an abort
  * gate-off ran clean    ->  gate-on rejecting it at compile time is
    SUSPICIOUS but not automatically wrong.  `TUR-E0371` is universally
    quantified over the function's inputs while the program only exercises the
    arguments it happens to pass, so a correct refutation can coexist with a
    clean run.  These are counted and saved for triage, never failed on.

Proving the fuzzer can fail
---------------------------
A fuzzer nobody has seen fail is a fuzzer that reports zero because it is
broken.  Two one-line sabotages, both verified to be caught at n=200 seed=41
where the shipped build reports zero:

  A. Congruence without evidence -- in `rt_binding_is_pure` (elab_fns.c),
     replace `return rt_pure_binding(&c, b);` with `return true;`.
     Reproduces the two historical soundness bugs.  Caught: 8/200.

  B. Entry-check elision done wrong -- at the top of `rt_inject_param_checks`,
     add `if (g_opt_refined) return body;`.  This SIMULATES the next planned
     feature (whole-program elision of a callee's entry check) implemented
     without the exported/address-taken analysis that would make it sound.
     Caught: 14/200.  Worth rerunning when that feature is actually built.

    cmake -S . -B build-unsound -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-unsound -j
    python3 tests/refine-fuzz-src.py --tur ./build-unsound/tur --n 200 --seed 41

`--self-test` runs a cheaper check of the plumbing (exit-code capture, stdout
capture, classification) against the pinned regression fixtures.

Usage
-----
    python3 tests/refine-fuzz-src.py [--n 300] [--seed 1] [--jobs 4]
                                     [--mode int|float|both]
                                     [--tur ./build/tur] [--save-dir DIR]
                                     [--self-test]

Exit status is 1 if any BUG class fired, 0 otherwise.  Suspicious cases alone
never fail the run.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Program generation
# ---------------------------------------------------------------------------

CMP_OPS = ["<", "<=", ">", ">=", "=", "not="]

# Every helper the generator can emit, tagged with whether calling it twice
# with equal arguments really does produce equal results.  `truly_pure` is the
# GROUND TRUTH, not what the compiler thinks -- the whole point is to include
# helpers that look pure and are not.
#
#   kind          arity  truly_pure  what it is
#   pure_plain      2      yes       ordinary arithmetic, no annotation
#   pure_fx         2      yes       same, but declares #fx{}
#   pure_rec        1      yes       recursive -- exercises the cycle rule
#   impure_c        1      NO        declares #fx{} and counts up in inline C
#   impure_c0       0      NO        nullary counter (the original repro shape)


class Gen:
    def __init__(self, rng, mode):
        self.rng = rng
        self.mode = mode            # "int" or "float"
        self.helpers = []           # list of (name, kind, arity)

    # -- type-dependent leaves ------------------------------------------------

    @property
    def ty(self):
        return "int" if self.mode == "int" else "float"

    def lit(self):
        if self.mode == "int":
            return str(self.rng.randint(-6, 6))
        # Non-zero fractional parts on purpose: an integral float literal
        # cannot show truncation or int/float divergence.
        return self.rng.choice(
            ["0.5", "1.25", "2.75", "3.25", "7.1", "-1.5", "-4.25", "6.5"])

    def nonzero_lit(self):
        if self.mode == "int":
            return str(self.rng.choice([-4, -3, -2, -1, 1, 2, 3, 4]))
        return self.rng.choice(["1.25", "2.5", "-3.5", "4.0"])

    # -- helpers --------------------------------------------------------------

    def gen_helpers(self, n):
        """Emit n helper defns.  Returns (source_lines, helper_specs)."""
        lines = []
        kinds = ["pure_plain", "pure_fx", "pure_rec", "impure_c", "impure_c0"]
        for i in range(n):
            kind = self.rng.choice(kinds)
            name = "h%d" % i
            if kind == "pure_plain":
                body = self.expr(2, ["a", "b"])
                lines.append("(defn %s [a : %s, b : %s] : %s\n  %s)"
                             % (name, self.ty, self.ty, self.ty, body))
                self.helpers.append((name, kind, 2, True))
            elif kind == "pure_fx":
                body = self.expr(2, ["a", "b"])
                lines.append("(defn %s [a : %s, b : %s] #fx{} : %s\n  %s)"
                             % (name, self.ty, self.ty, self.ty, body))
                self.helpers.append((name, kind, 2, True))
            elif kind == "pure_rec":
                # Structurally decreasing on the int path; on the float path a
                # plain guard, since float recursion is not the interesting bit.
                if self.mode == "int":
                    lines.append(
                        "(defn %s [a : int] #fx{} : int\n"
                        "  (if (<= a 0) 0 (+ 1 (%s (- a 1)))))" % (name, name))
                else:
                    lines.append(
                        "(defn %s [a : float] #fx{} : float\n"
                        "  (if (<= a 0.0) 0.0 (* a 0.5)))" % name)
                self.helpers.append((name, kind, 1, True))
            elif kind == "impure_c":
                # Declares an EMPTY effect row and is not remotely pure.  This
                # is the shape that broke the compiler twice.
                if self.mode == "int":
                    lines.append(
                        "(defn %s [a : int] #fx{} : int\n"
                        "  ```c\n  static int64_t s = 0;\n"
                        "  s += a + 1;\n  return s;\n  ```)" % name)
                else:
                    lines.append(
                        "(defn %s [a : float] #fx{} : float\n"
                        "  ```c\n  static double s = 0.0;\n"
                        "  s += a + 1.5;\n  return s;\n  ```)" % name)
                self.helpers.append((name, kind, 1, False))
            else:  # impure_c0
                if self.mode == "int":
                    lines.append(
                        "(defn %s [] #fx{} : int\n"
                        "  ```c\n  static int64_t n = 0;\n"
                        "  return n++;\n  ```)" % name)
                else:
                    lines.append(
                        "(defn %s [] #fx{} : float\n"
                        "  ```c\n  static double n = 0.0;\n"
                        "  n += 1.25;\n  return n;\n  ```)" % name)
                self.helpers.append((name, kind, 0, False))
        return lines

    # -- expressions ----------------------------------------------------------

    def expr(self, depth, scope):
        """An arithmetic expression over `scope` (a list of variable names)."""
        if depth <= 0 or self.rng.random() < 0.30:
            if scope and self.rng.random() < 0.6:
                return self.rng.choice(scope)
            return self.lit()
        r = self.rng.random()
        if r < 0.45:
            op = self.rng.choice(["+", "-", "*"])
            return "(%s %s %s)" % (op, self.expr(depth - 1, scope),
                                   self.expr(depth - 1, scope))
        if r < 0.55:
            # Divisor is a nonzero literal: a division trap would abort both
            # gates identically, which is noise rather than signal.
            return "(/ %s %s)" % (self.expr(depth - 1, scope), self.nonzero_lit())
        if r < 0.95 and self.helpers:
            name, _kind, arity, _pure = self.rng.choice(self.helpers)
            args = " ".join(self.expr(depth - 1, scope) for _ in range(arity))
            return "(%s%s%s)" % (name, " " if args else "", args)
        return self.expr(depth - 1, scope)

    # -- predicates -----------------------------------------------------------

    def atom(self, scope):
        op = self.rng.choice(CMP_OPS)
        return "(%s %s %s)" % (op, self.expr(2, scope), self.expr(1, scope))

    def pred(self, depth, scope):
        if depth <= 0 or self.rng.random() < 0.55:
            return self.atom(scope)
        r = self.rng.random()
        if r < 0.4:
            return "(and %s %s)" % (self.pred(depth - 1, scope),
                                    self.pred(depth - 1, scope))
        if r < 0.8:
            return "(or %s %s)" % (self.pred(depth - 1, scope),
                                   self.pred(depth - 1, scope))
        return "(not %s)" % self.pred(depth - 1, scope)

    # -- whole program --------------------------------------------------------
    #
    # Three shapes, because uniform random generation almost never lands inside
    # the solver's fragment: a 40-case smoke run proved 2 obligations, and the
    # soundness property only has teeth on a case where a check was actually
    # ELIDED.  `linear` and `congruence` are built to be decidable often --
    # sometimes truly, sometimes falsely, which is the point.

    def _target(self, params, ret_pred, pre, body):
        sig = ", ".join("%s : %s" % (p, self.ty) for p in params)
        return ("(defn target [%s] : #refine{ r : %s | %s }%s\n  %s)"
                % (sig, self.ty, ret_pred,
                   "\n  :pre " + pre if pre else "", body))

    def _main(self, params):
        calls = []
        for _ in range(self.rng.randint(1, 3)):
            args = " ".join(self.lit() for _ in params)
            calls.append("  (println (target%s%s))" % (" " if args else "", args))
        return "(defn main [] : int\n%s\n  0)" % "\n".join(calls)

    def shape_random(self):
        """Anything goes.  Mostly lands outside the fragment; that is coverage
        of the fallback path, which must never elide."""
        params = ["p%d" % i for i in range(self.rng.randint(0, 2))]
        return (params,
                self._target(params,
                             self.pred(2, params + ["r"]),
                             self.pred(1, params) if params and self.rng.random() < 0.5 else None,
                             self.expr(3, params)))

    def shape_linear(self):
        """Linear arithmetic over one or two params: a body the encoder fully
        understands, a `:pre` that bounds the inputs, and a goal relating `r`
        to them.  S2 decides most of these outright."""
        params = ["p%d" % i for i in range(self.rng.randint(1, 2))]
        p0 = params[0]
        k = self.lit()
        body = self.rng.choice([
            p0,
            "(+ %s %s)" % (p0, k),
            "(- %s %s)" % (p0, k),
            "(* %s 2)" % p0 if self.mode == "int" else "(* %s 2.0)" % p0,
            "(+ %s %s)" % (p0, params[-1]),
        ])
        lo, hi = sorted([self.lit(), self.lit()])
        pre = self.rng.choice([
            "(>= %s %s)" % (p0, lo),
            "(and (>= %s %s) (<= %s %s))" % (p0, lo, p0, hi),
            "(< %s %s)" % (p0, hi),
            None,
        ])
        rhs = self.rng.choice([self.lit(), p0, "(+ %s %s)" % (p0, self.lit())])
        ret_pred = "(%s r %s)" % (self.rng.choice(CMP_OPS), rhs)
        if self.rng.random() < 0.25:
            ret_pred = "(and %s (%s r %s))" % (
                ret_pred, self.rng.choice(CMP_OPS), self.lit())
        return params, self._target(params, ret_pred, pre, body)

    def shape_congruence_method(self):
        """The bug shape aimed at a TYPECLASS METHOD.

        A method has no global binding under its bare name, so it used to fall
        through to the abstract-measure rule -- "a name that resolves to no
        function at all is an uninterpreted mathematical function, therefore
        congruent" -- and pick up congruence for free even when the instance
        was wired to a counter.  The plain `congruence` shape could not reach
        this: it only ever repeats a call to a HELPER, and `typeclass` never
        repeats a call at all.  The bug lived in the gap between two shapes
        that each looked like they covered it."""
        impure = [h for h in self.helpers if not h[3]]
        pure = [h for h in self.helpers if h[3]]
        pool = impure if (impure and self.rng.random() < 0.7) else (pure or impure)
        if not pool:
            return self.shape_congruence()
        hname, _k, harity, _p = self.rng.choice(pool)
        inner = "(%s%s)" % (hname, " self" * harity)
        z = self._zero()
        call = self.rng.choice(["(.tickm %s)", "(tickm %s)"]) % ("1" if self.mode == "int" else "1.25")
        lines = [
            "(defclass Ticker [a]\n  (tickm [self : a] : %s))" % self.ty,
            "(definstance Ticker [%s]\n  (tickm [self : %s] : %s\n    %s))"
            % (self.ty, self.ty, self.ty, inner),
            "(defn target [] : #refine{ r : %s | %s }\n  (- %s %s))"
            % (self.ty, self.rng.choice(["(>= r %s)" % z, "(= r %s)" % z,
                                         "(<= r %s)" % z]), call, call),
        ]
        return [], "\n\n".join(lines)

    def shape_congruence(self):
        """The bug shape.  Two occurrences of the SAME call, subtracted, with a
        refinement that holds iff the callee is genuinely pure.  Half the
        helpers available here are liars."""
        name, _kind, arity, truly_pure = self.rng.choice(self.helpers)
        params = ["p0"] if arity >= 1 else []
        # Every argument is `p0`, so the two occurrences are textually and
        # semantically identical -- which is exactly what congruence claims.
        # Saturate the call: under-applying would build a closure and the
        # program would not compile.
        call = "(%s%s)" % (name, " p0" * arity)
        body = self.rng.choice([
            "(- %s %s)" % (call, call),
            "(- (+ %s %s) (* 2 %s))" % (call, call, call) if self.mode == "int"
            else "(- (+ %s %s) (* 2.0 %s))" % (call, call, call),
        ])
        zero = "0" if self.mode == "int" else "0.0"
        ret_pred = self.rng.choice([
            "(>= r %s)" % zero, "(= r %s)" % zero, "(<= r %s)" % zero])
        # truly_pure is ground truth the generator knows and the compiler must
        # work out for itself; it is not written into the program.
        _ = truly_pure
        return params, self._target(params, ret_pred, None, body)

    def _zero(self):
        return "0" if self.mode == "int" else "0.0"

    def bound(self, var):
        """A simple, decidable bound on `var` -- the kind of hypothesis a
        refined parameter is actually written to supply."""
        z, k = self._zero(), self.lit()
        return self.rng.choice([
            "(>= %s %s)" % (var, z),
            "(> %s %s)" % (var, z),
            "(<= %s %s)" % (var, k),
            "(and (>= %s %s) (<= %s %s))" % (var, z, var, self.lit()),
        ])

    def shape_param(self):
        """A refined PARAMETER feeding a refined return.

        Entry checks are never elided, so this shape cannot go wrong by
        elision on its own -- but the parameter predicate becomes a HYPOTHESIS
        for the return obligation, and the return check IS elided when that
        obligation discharges. If the hypothesis were ever assumed without the
        check that backs it, this is the shape that would show it."""
        pp = self.bound("v")
        body = self.rng.choice([
            "p0",
            "(+ p0 %s)" % self.lit(),
            "(* p0 2)" if self.mode == "int" else "(* p0 2.0)",
            "(- p0 %s)" % self.lit(),
        ])
        rp = "(%s r %s)" % (self.rng.choice(CMP_OPS),
                            self.rng.choice([self._zero(), self.lit(), "p0"]))
        return (["p0"],
                "(defn target [p0 : #refine{ v : %s | %s }] : #refine{ r : %s | %s }\n  %s)"
                % (self.ty, pp, self.ty, rp, body))

    def shape_propagate(self):
        """RT4 result propagation across a call-site crossing: a refined result
        flows into a refined parameter, through a caller that has its own
        refinement. Three obligations per program, all in the elaboration
        layer -- the layer both known soundness bugs lived in."""
        inner_pp = self.bound("v")
        inner_rp = "(%s r %s)" % (self.rng.choice([">", ">=", "<", "<="]),
                                  self.rng.choice([self._zero(), self.lit()]))
        inner_body = self.rng.choice(
            ["x", "(+ x %s)" % self.lit(),
             "(* x 2)" if self.mode == "int" else "(* x 2.0)"])
        outer_pp = self.bound("w")
        fwd = self.rng.choice(["y", "(+ y %s)" % self.lit()])
        lines = [
            "(defn inner [x : #refine{ v : %s | %s }] : #refine{ r : %s | %s }\n  %s)"
            % (self.ty, inner_pp, self.ty, inner_rp, inner_body),
            "(defn outer [y : #refine{ w : %s | %s }] : %s\n  (inner %s))"
            % (self.ty, outer_pp, self.ty, fwd),
            "(defn target [p0 : %s] : %s\n  (outer p0))" % (self.ty, self.ty),
        ]
        return ["p0"], "\n\n".join(lines)

    def shape_typeclass(self):
        """Typeclass method dispatch and refinement variance.

        Parameters and results vary in OPPOSITE directions, so both ladders are
        generated: for a parameter an instance may demand LESS than the class
        (legal) but not more (`TUR-E0374`); for a result it may deliver MORE
        (legal) but not less. Both dispatch spellings -- dotted `(.m x k)` and
        bare `(m x k)` -- resolve to the same instance method and must be
        checked identically."""
        z = self._zero()
        # (class_pred, instance_pred, legal?) -- an instance demanding MORE of a
        # parameter than the class promises is the illegal direction.
        cp, ip = self.rng.choice([
            ("(>= v %s)" % z, "(>= v %s)" % z),   # identical
            ("(> v %s)" % z,  "(>= v %s)" % z),   # instance demands less: legal
            ("(>= v %s)" % z, "(> v %s)" % z),    # instance demands more: E0374
        ])
        # Result ladder, the other way round: promising LESS than the class is
        # the illegal direction.  The last entry is the one that matters most:
        # a NONLINEAR instance promise makes `instance |- class` abstract to an
        # uninterpreted term, so the variance check answers unknown and emits
        # no error -- while a caller is still handed the class promise. Only
        # the extra class check injected on the instance keeps that sound, so
        # this rung is what exercises it.
        use_ret = self.rng.random() < 0.6
        nonlin = ("(>= (* r r) (* r 2))" if self.mode == "int"
                  else "(>= (* r r) (* r 2.0))")
        c_rp, i_rp = self.rng.choice([
            ("(>= r %s)" % z, None),              # instance inherits
            ("(>= r %s)" % z, "(>= r %s)" % z),   # restated verbatim
            ("(>= r %s)" % z, "(> r %s)" % z),    # delivers more: legal
            ("(>= r %s)" % z, "(>= r -5)" if self.mode == "int"
                              else "(>= r -5.5)"),  # delivers less: E0374
            ("(>= r %s)" % z, nonlin),            # weaker, but undecidably so
        ])
        cls_ret = (" : #refine{ r : %s | %s }" % (self.ty, c_rp)) if use_ret else " : %s" % self.ty
        if use_ret and i_rp:
            inst_ret = " : #refine{ r : %s | %s }" % (self.ty, i_rp)
        else:
            inst_ret = " : %s" % self.ty
        body = self.rng.choice([
            "(* self k)", "(+ self k)",
            "(- self k)", "(+ (* self k) 1)" if self.mode == "int"
                          else "(+ (* self k) 1.25)"])
        call = self.rng.choice(["(.meth p0 %s)", "(meth p0 %s)"]) % self.lit()
        # Give the caller its own refinement half the time: that is what makes
        # RT4 propagate the CLASS promise across the dispatch and elide the
        # caller's check, which is the path where a wrong promise turns into a
        # wrong answer rather than just a missing hypothesis.
        if use_ret and self.rng.random() < 0.5:
            tgt_ret = "#refine{ r : %s | %s }" % (
                self.ty, self.rng.choice(["(>= r %s)" % z, "(> r %s)" % z]))
        else:
            tgt_ret = self.ty
        lines = [
            "(defclass Cls [a]\n  (meth [self : a, k : #refine{ v : %s | %s }]%s))"
            % (self.ty, cp, cls_ret),
            "(definstance Cls [%s]\n  (meth [self : %s, k : #refine{ v : %s | %s }]%s\n    %s))"
            % (self.ty, self.ty, self.ty, ip, inst_ret, body),
            "(defn target [p0 : %s] : %s\n  %s)" % (self.ty, tgt_ret, call),
        ]
        return ["p0"], "\n\n".join(lines)

    def program(self):
        lines = self.gen_helpers(self.rng.randint(1, 3))
        r = self.rng.random()
        if r < 0.20:
            params, target = self.shape_random()
        elif r < 0.42:
            params, target = self.shape_linear()
        elif r < 0.60:
            params, target = self.shape_congruence()
        elif r < 0.74:
            params, target = self.shape_param()
        elif r < 0.80:
            params, target = self.shape_propagate()
        elif r < 0.90:
            params, target = self.shape_typeclass()
        else:
            params, target = self.shape_congruence_method()
        lines.append(target)
        lines.append(self._main(params))
        return "\n\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Running one case
# ---------------------------------------------------------------------------

TIMEOUT = 60

STATS_RE = re.compile(
    r"refine: (\d+) obligation\(s\): (\d+) proven, (\d+) refuted, (\d+) unknown")


class Outcome:
    """What one `tur run` did.  `kind` is one of clean / abort / reject / timeout."""

    def __init__(self, kind, stdout="", proven=0, refuted=0):
        self.kind = kind
        self.stdout = stdout
        self.proven = proven
        self.refuted = refuted


def run_gate(tur, path, refined):
    cmd = [tur, "run"]
    if refined:
        cmd.append("--enable=refined")
    cmd.append(path)
    env = dict(os.environ)
    env["TUR_REFINE_STATS"] = "1"
    # The generated programs are tiny and self-contained; leak checking the
    # spawned binary is not what this harness is measuring.
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") or "detect_leaks=0"
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")

    proven = refuted = 0
    m = STATS_RE.search(p.stderr)
    if m:
        proven, refuted = int(m.group(2)), int(m.group(3))

    if p.returncode == 0:
        return Outcome("clean", p.stdout, proven, refuted)
    # SIGABRT (134) and SIGSEGV (139) both mean the program was built and then
    # died; a contract violation is the 134 case.
    if p.returncode in (134, 139) or p.returncode < 0:
        return Outcome("abort", p.stdout, proven, refuted)
    return Outcome("reject", p.stdout, proven, refuted)


# Classification.  BUG_* fail the run; SUSPICIOUS is reported only.
BUGS = ("BUG_soundness", "BUG_output_divergence", "BUG_new_abort")


def classify(off, on):
    if off.kind == "timeout" or on.kind == "timeout":
        return "skip_timeout"
    if off.kind == "reject":
        # The generator emitted something that does not compile at all.  Not a
        # refinement result; drop the case.
        return "skip_invalid"

    if off.kind == "clean":
        if on.kind == "clean":
            return ("agree_clean" if off.stdout == on.stdout
                    else "BUG_output_divergence")
        if on.kind == "abort":
            return "BUG_new_abort"
        return "SUSPICIOUS_over_refute"

    # off.kind == "abort": the program violates its own refinement.
    if on.kind == "clean":
        return "BUG_soundness"
    if on.kind == "abort":
        return "agree_abort"
    return "agree_rejected_early"


def one_case(tur, workdir, idx, seed, mode):
    rng = random.Random(seed)
    src = Gen(rng, mode).program()
    # Gate-off and gate-on need DISTINCT paths: `tur run` derives its
    # intermediate .c name from the source path, so a shared name would let two
    # workers (or the two gates) collide in /tmp/tur-build.
    base = os.path.join(workdir, "c%06d" % idx)
    off_path, on_path = base + "_off.tur", base + "_on.tur"
    for p in (off_path, on_path):
        with open(p, "w") as f:
            f.write(src)
    off = run_gate(tur, off_path, False)
    on = run_gate(tur, on_path, True)
    for p in (off_path, on_path):
        try:
            os.unlink(p)
        except OSError:
            pass
    return classify(off, on), src, off, on


# ---------------------------------------------------------------------------
# Self-test: does the plumbing classify the pinned fixtures correctly?
# ---------------------------------------------------------------------------

SELF_TESTS = [
    # (fixture, expected classification)
    ("tests/fixtures/errors/refine-impure-fx-empty/input.tur",
     "agree_rejected_early"),
    ("tests/fixtures/errors/refine-impure-not-congruent/input.tur",
     "agree_rejected_early"),
    ("tests/fixtures/refine-proved/input.tur", "agree_clean"),
    ("tests/fixtures/refine-measure-euf/input.tur", "agree_clean"),
    ("tests/fixtures/refine-class-result/input.tur", "agree_clean"),
    # Runs CLEAN with the gate off -- its single call returns 7, which satisfies
    # the instance's weaker promise -- and is rejected with the gate on, because
    # E0374 is a declaration-vs-declaration inconsistency rather than a claim
    # about the value this program produced. That is `SUSPICIOUS_over_refute` by
    # definition, and pinning it here documents that the bucket holds legitimate
    # DECLARATION errors too, not only universal value refutations.
    ("tests/fixtures/errors/refine-instance-result-weaker/input.tur",
     "SUSPICIOUS_over_refute"),
]


def self_test(tur):
    ok = True
    for rel, want in SELF_TESTS:
        path = os.path.join(REPO, rel)
        off = run_gate(tur, path, False)
        on = run_gate(tur, path, True)
        got = classify(off, on)
        status = "ok " if got == want else "FAIL"
        if got != want:
            ok = False
        print("  %s %-52s want=%s got=%s (off=%s on=%s)"
              % (status, rel.split("/")[-2], want, got, off.kind, on.kind))
    # The two regression fixtures must ALSO show that gate-off actually aborts
    # -- that is the half of the property the classification alone does not
    # pin, since `reject` under both gates would look the same.
    for rel, _ in SELF_TESTS[:2]:
        off = run_gate(tur, os.path.join(REPO, rel), False)
        if off.kind != "abort":
            print("  FAIL %s: gate-off should abort, got %s" % (rel, off.kind))
            ok = False
    return ok


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=300, help="cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--mode", choices=["int", "float", "both"], default="both")
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None,
                    help="where to write failing cases (default: a temp dir)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    tur = os.path.abspath(args.tur)
    if not os.path.exists(tur):
        print("refine_fuzz_src: no compiler at %s" % tur, file=sys.stderr)
        return 2

    if args.self_test:
        print("refine_fuzz_src: self-test (%s)" % tur)
        ok = self_test(tur)
        print("refine_fuzz_src: self-test %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    save_dir = args.save_dir or tempfile.mkdtemp(prefix="refine-fuzz-src-")
    os.makedirs(save_dir, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="refine-fuzz-work-")

    counts = {}
    findings = []
    total_proven = total_refuted = 0

    def job(i):
        mode = args.mode
        if mode == "both":
            mode = "int" if (i % 2 == 0) else "float"
        return one_case(tur, workdir, i, args.seed * 1000003 + i, mode)

    print("refine_fuzz_src: %d cases, seed %d, mode %s, %d job(s)"
          % (args.n, args.seed, args.mode, args.jobs))
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for i, (kind, src, off, on) in enumerate(
                    pool.map(job, range(args.n))):
                counts[kind] = counts.get(kind, 0) + 1
                total_proven += on.proven
                total_refuted += on.refuted
                if kind.startswith("BUG") or kind.startswith("SUSPICIOUS"):
                    findings.append((kind, i, src, off, on))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    for kind, i, src, off, on in findings:
        d = os.path.join(save_dir, "%s-%06d" % (kind, i))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "input.tur"), "w") as f:
            f.write(src)
        with open(os.path.join(d, "README"), "w") as f:
            f.write("classification: %s\nseed base: %d  case: %d\n"
                    "gate-off: %s\n  stdout: %r\n"
                    "gate-on : %s\n  stdout: %r\n"
                    % (kind, args.seed, i, off.kind, off.stdout,
                       on.kind, on.stdout))

    print("\nrefine_fuzz_src: %d cases" % args.n)
    for k in sorted(counts):
        print("  %-28s : %d" % (k, counts[k]))
    print("  %-28s : %d proven, %d refuted"
          % ("obligations decided", total_proven, total_refuted))

    n_bugs = sum(counts.get(b, 0) for b in BUGS)
    n_susp = counts.get("SUSPICIOUS_over_refute", 0)
    print("\n  SOUNDNESS bugs              : %d" % counts.get("BUG_soundness", 0))
    print("  other BUG classes           : %d" % (n_bugs - counts.get("BUG_soundness", 0)))
    print("  suspicious (report-only)    : %d" % n_susp)
    if findings:
        print("\n  saved to %s" % save_dir)
    elif not args.save_dir:
        shutil.rmtree(save_dir, ignore_errors=True)

    if total_proven == 0:
        print("\n  WARNING: nothing was proven in any case -- the solver never "
              "engaged, so a clean run means very little.")
    return 1 if n_bugs else 0


if __name__ == "__main__":
    sys.exit(main())
