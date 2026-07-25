# Refinement Types

> **Status:** prototype, behind the `refined` experiment.
> Enable with `--enable=refined` on the command line, `:experiments [:refined]`
> in `build.tur`, or `#lang turmeric refined` at the top of a single file.
> See [contract-types-guide.md](contract-types-guide.md) for the always-on
> runtime half, and
> [../upcoming/v1/refinement-types-plan.md](../upcoming/v1/refinement-types-plan.md)
> for the design.

Contract types (`#refine{ x : T | p }`) check their predicate at **runtime**.
Refinement types go one step further: with the `refined` experiment on, the
compiler tries to **prove** the predicate at compile time, and emits a runtime
check only where the proof fails.

The single fact that shapes the whole feature: **every refinement already has a
runtime meaning.** So the static discharger is allowed to give up on any
obligation and stay sound -- the obligation just falls back to the check it
would have had anyway. That is why a partial, hand-rolled solver is a real
feature rather than a broken one, and why turning `refined` on can never make a
correct program wrong.

```turmeric
;; Proved: x > 0 entails 2x > 0, so no runtime check is emitted for the return.
(defn double-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (* x 2))
```

---

## Turning it on

| Spelling | Scope |
|---|---|
| `--enable=refined` | this compiler invocation |
| `:experiments [:refined]` in `build.tur` | the project |
| `#lang turmeric refined` (first line of a file) | that file |
| `~/.config/turmeric/experiments.tur` `:enable [:refined]` | the user |

`#lang turmeric refined` is **exactly** `--enable=refined` scoped to one file --
the `#lang` layer points at the same `EXPERIMENTS[]` row, so there is one enable
path, one lifecycle warning, and one `expires_at`. If a project manifest states
its own `:experiments` list and leaves `refined` out, a `#lang ... refined` file
is a **hard error**, never a silent downgrade: the project owner said no, and
compiling the file under different semantics than it asked for would be worse
than failing.

With the experiment off, everything below still parses and still runs its
runtime checks. Nothing here is required to use contract types.

### `--strict-refine`

`--strict-refine` is a diagnostic-strictness knob, not an experiment. It turns
every obligation the solver could not prove into a hard compile error instead of
a warning plus a runtime check. Use it when you want a build in which *every*
refinement is discharged statically:

```sh
tur build --enable=refined --strict-refine src/main.tur
```

---

## What gets checked

Two things become **hypotheses** -- facts the prover may assume:

- each parameter declared with a contract type (or a named refinement alias);
- the function's `:pre` predicate.

Three things become **goals** -- things it must prove:

- a contract **return type**, `: #refine{ r : T | p }`;
- the function's `:post` predicate;
- every **argument** passed where the parameter declares a refinement.

```turmeric
(defn index-ok [v : int, n : int, i : int] : #refine{ r : int | (< r (size-of v)) }
  :pre (and (= (size-of v) n) (>= i 0) (< i n))
  i)
```

Here the `:pre` clause supplies three hypotheses, and the return refinement is
the goal. It is discharged statically, so the return check disappears; the
`:pre` clause itself is still checked at runtime, since it constrains the
caller rather than the body.

When a return or postcondition goal is proved, **no runtime check is emitted
for it**. When it is not, the check stays exactly where it was.

### Call sites

Passing a value where the parameter declares a refinement is a crossing into
it, so the *caller* owes the proof:

```turmeric
(load "stdlib/refine.tur")

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))

(safe-div 10 2)   ; proved: 2 != 0
(safe-div 10 0)   ; error[TUR-E0371] -- a compile failure, not a runtime panic
```

Definition order does not matter. Crossings are resolved after the whole
compilation unit is elaborated, so a call to a function defined later in the
file is checked exactly like a call to one defined earlier.

A predicate may mention a **sibling parameter**, and the call site substitutes
the argument in that slot rather than leaving a free variable:

```turmeric
(defn at [n : int, i : #refine{ j : int | (and (>= j 0) (< j n)) }] : int ...)

(at 10 3)   ; proved: 0 <= 3 < 10
(at 3 10)   ; error[TUR-E0371]
```

**An argument the solver cannot prove is not an error by default.** Only an
argument that is *definitely* wrong -- one whose goal is closed, so it
evaluates to false on the values written at the site -- fails the build. That
distinction is deliberate: the callee still checks its parameters on entry, and
erroring on every argument whose value is not statically known would make the
experiment impossible to adopt incrementally. `--strict-refine` opts into the
stricter reading, where any undischarged argument is a hard error.

The callee's **entry check is always emitted**, even when every visible call
site is proved. Eliding it would need whole-program knowledge of the call graph
(including exported and indirect callers), and getting that wrong drops a check
that was protecting something. The call-site layer is a diagnostic on top of
that guard, not a licence to remove it.

### Indirect callees

A call does not have to name a global function to be checked. Two shapes carry
their parameter refinements to the call site:

```turmeric
;; a lambda with contract parameters
(let [f (fn [x : #refine{ v : int | (> v 0) }] : int (* x 2))]
  (f 21))     ; proved
  ;; (f 0)   -- error[TUR-E0371]

;; an alias of a global function
(let [g safe-div]
  (g 10 0))   ; error[TUR-E0371] -- the alias resolves to safe-div
```

A lambda's contract parameter gets an entry check exactly like a `defn`'s, and
a captured value's own refinement is in scope as a hypothesis inside the
lambda body.

A **typeclass method** is checked too, when the dispatch resolves to a known
instance:

```turmeric
(defclass Scaler [a]
  (scale-by [self : a, k : #refine{ v : int | (> v 0) }] : int))

(.scale-by 3 4)   ; proved
(.scale-by 3 0)   ; error[TUR-E0371] on argument 2 of 'scale-by'
```

Both the dotted `(.m x ...)` and bare `(m x ...)` forms go through the same
resolution and are checked identically. A dispatch that stays dynamic -- no
statically-selected instance -- is not checked, because which method runs is
not known at the site; the method's own entry check still guards it.

An instance may accept **more** than its class signature promises, but not
less. The class signature is the contract callers program against, so an
instance that demanded more would reject an argument a generic caller was
entitled to pass:

```turmeric
(defclass Scaler [a]
  (scale-by [self : a, k : #refine{ v : int | (>= v 0) }] : int))

;; error[TUR-E0374]: rejects 0, which the class admits
(definstance Scaler [int]
  (scale-by [self : int, k : #refine{ v : int | (> v 0) }] : int ...))

;; fine: accepts everything the class promises, and more
(definstance Scaler [int]
  (scale-by [self : int, k : int] : int ...))
```

A class parameter with no refinement promises nothing, so *any* instance
refinement on it is a strengthening. The check is `class_pred |- instance_pred`
through the same solver seam, and it reports only on a refutation -- an
undecidable pair keeps the runtime check.

What is *not* checked is a genuinely higher-order callee -- see
[Limits](#limits).

### Results carry their refinements

A call is an opaque term to the solver -- but when the callee has a return
refinement, that predicate is a *fact about the value this call produced*, so
it is asserted. Refined code composes instead of going opaque at every call:

```turmeric
(defn double-pos [x : Pos] : #refine{ r : int | (> r 0) }
  (* x 2))

(defn twice [y : Pos] : int (* y 2))

(defn use [p : Pos] : int
  (twice (double-pos p)))    ; proved -- double-pos's result is known positive
```

Assuming a declared return refinement is sound because something enforces it:
either it was proved statically, or the runtime check that guarantees it would
have panicked first. When neither is true -- contracts stripped by
`--no-contracts`, or a release build without `--keep-contracts` -- the
refinement is **not** published, and call sites go back to treating the result
as opaque.

### Inferred result refinements

A function with no declared return refinement gets one tried for it. RT4 runs a
small fixed vocabulary of shapes (`> 0`, `< 0`, `>= 0`, `<= 0`, `!= 0`) against
the body and keeps the first a backend **proves**:

```turmeric
(defn inc-pos [x : Pos] : int
  (+ x 1))          ; (> result 0) is inferred and published to call sites

(defn use [p : Pos] : int
  (twice (inc-pos p)))    ; proved, with no annotation on inc-pos at all
```

This is not general refinement inference. There is no search over a recursive
constraint system -- the layer that makes a LiquidHaskell-style backend hard is
exactly the one Turmeric deletes by making refinements *written*. These are a
handful of guesses, each individually checked, so a wrong guess costs a
discarded proof attempt and never a wrong answer. A function whose result
genuinely satisfies none of them simply gets nothing:

```turmeric
(defn lower [x : Pos] : int (- x 5))   ; can be negative -- nothing inferred
(defn use [p : Pos] : int (twice (lower p)))   ; correctly stays undischarged
```

Scope, deliberately narrow: single-expression bodies, a numeric result, at most
four refined parameters. Branching bodies need a path-sensitive join at the
merge point and are deferred. `TUR_REFINE_STATS=1` reports how many
refinements were inferred and how many probes it took.

---

## The predicate language

The supported fragment is quantifier-free linear integer/real arithmetic with
equality and uninterpreted functions.

```
pred ::= (= e e) | (not= e e) | (< e e) | (<= e e) | (> e e) | (>= e e)
       | (and pred ...) | (or pred ...) | (not pred) | (=> pred pred)
       | (measure e ...)          ; a named measure -> an uninterpreted function
       | true | false

expr ::= <int literal> | <float literal>
       | <the refinement's bound variable>
       | <any parameter in scope>
       | (+ e e) | (- e e) | (* e <literal>) | (/ e <literal>) | (mod e <literal>)
```

Two rules keep this on the cheap side of the solver cliff. They are rules, not
accidents:

**Named measures are uninterpreted functions.** Any call the encoder does not
recognise -- `len`, `size-of`, your own helper -- becomes an opaque symbol that
congruence closure reasons about but never unfolds. This is what makes the
equality theory tractable, and it is why `(= (size-of v) n)` above is usable as
a hypothesis without the solver knowing anything about `size-of`'s body.

**A measure must be provably pure.** Congruence -- treating two occurrences of
`(size-of v)` as the same value -- is only valid for a pure function, and the
compiler decides that for itself by walking the callee's body:

```turmeric
(defn size-of [v : int] #fx{} : int
  (* v 2))
```

The walk is **default-deny**. It admits literals, reads of immutable bindings,
`if` / `let` / `do` / `return`, the arithmetic, comparison, and logical
builtins, and direct calls to functions that are themselves pure -- recursion
included, so a recursive measure still gets congruence. Everything else is
impure: inline C, `set!`, `perform`, dereferences, field reads, closures,
indirect and rank-2-polymorphic calls, the `println` family, raw memory, and
any callee whose body is not available (an `extern`, or a forward reference not
yet elaborated). Without a purity proof each occurrence is a *distinct* opaque
value and the proof does not go through.

A declared effect row is a **veto, not evidence**. `#fx{Log}` rules purity out;
`#fx{}` on its own proves nothing, because the effect system tracks *algebraic*
effects and infers nothing from `set!`, from a mutable global, or from inline C.
A function can declare an empty row, keep a `static` counter in a C block, and
return a different value on every call.

The rule has teeth. Given a `tick` that counts up, `(- (tick) (tick))` is `-1`,
never `0` -- but encoded congruently it becomes `t - t`, and a refinement of
`(>= r 0)` on it would be "proved" and its check elided. Both halves of that
hole are pinned by regression fixtures
(`errors/refine-impure-not-congruent`, `errors/refine-impure-fx-empty`).

The asymmetry is the point: a case the walk has not learned costs one runtime
check, while a wrong purity guess elides a check that was protecting something.
A name that resolves to no function at all is still treated as congruent: that
is an abstract measure, a mathematical function by definition rather than code
that runs.

**Variable * variable is uninterpreted.** `(* x 2)` is linear and fully decided;
`(* x y)` with both sides variable is abstracted to an opaque term and reported
with `TUR-W0373`. Congruence closure still relates two occurrences of the same
product, so nothing becomes unsound -- the arithmetic facts are simply gone, and
the obligation falls back to its runtime check.

```turmeric
;; TUR-W0373 + TUR-W0372: true, but not provable in the linear fragment.
(defn mul-pos [x : #refine{ v : int | (> v 0) }, y : #refine{ w : int | (> w 0) }]
             : #refine{ r : int | (> r 0) }
  (* x y))
```

We do not climb the nonlinear wall. A genuinely nonlinear obligation gets a
runtime check, and that is the intended outcome.

---

## Named refinements: `stdlib/refine.tur`

A `deftype` bound to a contract type is a **refinement alias**:

```turmeric
(deftype Pos #refine{ x : int | (> x 0) })
```

A parameter declared `[n : Pos]` takes the base `int` at the C level, gets
`(> n 0)` as its entry check, and contributes that predicate as a hypothesis.

`stdlib/refine.tur` ships the common ones. It is **load-on-demand**, not
auto-loaded -- names like `Byte` and `Percent` do not belong in every program's
type namespace by default:

```turmeric
(load "stdlib/refine.tur")

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))
```

| Alias | Predicate |
|---|---|
| `Nat` | `(>= x 0)` |
| `Pos` | `(> x 0)` |
| `Neg` | `(< x 0)` |
| `NonZero` | `(not= x 0)` |
| `Byte` | `0 <= x <= 255` |
| `Percent` | `0 <= x <= 100` |
| `NonNegFloat` | `(>= x 0.0)` |
| `PosFloat` | `(> x 0.0)` |
| `UnitFloat` | `0.0 <= x <= 1.0` |

A refinement alias takes no type parameters in this prototype.

---

## The solver

There is **no heavyweight solver dependency**. The shipped compiler carries a
small, staged, in-house decision procedure, which is also why the WASM
playground gets static checking at zero download cost. Obligations run through
the stages in ascending cost and stop at the first one that decides:

| Stage | What it decides |
|---|---|
| **S0** trivial | constant goals, a goal that is syntactically a hypothesis, contradictory hypotheses |
| **S1** congruence closure (EUF) | equality and uninterpreted functions -- measures, abstracted products |
| **S2** linear arithmetic | conjunctions of linear constraints, by Fourier-Motzkin elimination over exact rationals (a superset of difference logic) |
| **S3** Nelson-Oppen | mixed goals, by exchanging entailed equalities between S1 and S2 |

Anything none of them decides is answered *unknown* and keeps its runtime check.
Every internal cap -- cube count, variable count, constraint growth -- degrades
the same way. The invariant the whole design rests on is one-directional: a
stage may never say "valid" for something that is not, but saying "unknown" is
always allowed.

Integers get one extra step: a strict constraint over integral data is tightened
(`e < 0` becomes `e <= -1`), which is what lets `x > 0` entail `2x > 0`. Full
integer completeness (branch-and-bound, Omega) is deliberately not attempted.

### Counterexamples

Failing to *prove* something proves nothing, so a separate bounded search tries
to **refute** the obligation: it enumerates a small candidate assignment space
and evaluates the formula exactly. A satisfying assignment is a real
counterexample, so it is reported with a model rather than a shrug.

The search declines VCs containing uninterpreted symbols -- a measure has no
fixed interpretation to evaluate, so guessing one would be dishonest.

### What a failure tells you

A failing obligation reports in three parts: the claim, a witness against it,
and a remedy.

```
error[TUR-E0371]: refinement on the return value of 'wrong' cannot be proved statically
note: the predicate (> r 0) does not hold for every input here
note: counterexample: x = -2
help: (> x 0) would discharge it -- e.g. declare x : #refine{ v : int | (> v 0) }
```

The `help:` line is **not a heuristic**. It is a second query through the same
solver seam: a candidate fact is asserted as a hypothesis and the chain is
asked again, so only a candidate that genuinely discharges the goal is ever
offered. Two families are tried -- comparisons against the literals the code
already mentions, and relations between two variables, which is what produces
an index bound:

```
error[TUR-E0371]: refinement on the return value of 'at' cannot be proved statically
note: the predicate (< r n) does not hold for every input here
help: (< i n) would discharge it -- e.g. declare i : #refine{ v : int | (< v n) }
```

A candidate that *contradicts* what is already known is rejected explicitly.
Without that check a contradictory hypothesis would discharge the goal by ex
falso, and the compiler would cheerfully suggest constraining a variable to be
both negative and positive. When nothing consistent helps, no `help:` line is
printed -- silence beats a nonsense suggestion:

```turmeric
(defn impossible [x : #refine{ v : int | (< v 0) }] : #refine{ r : int | (> r 0) }
  x)      ; reported, with a counterexample, and NO hint -- none exists
```

When the values are written right at the site, the wording sharpens
accordingly -- this is not "not for every input", it is "not for this one":

```
error[TUR-E0371]: refinement on argument 2 of 'safe-div' in 'main' cannot be proved statically
note: the predicate (not= x 0) is false for the value given here
```

---

## Diagnostics

| Code | Meaning |
|---|---|
| `TUR-E0371` | the predicate genuinely does not hold: a function's own claim is falsifiable, or an argument is definitely wrong at its call site |
| `TUR-W0372` | nothing decided it; the runtime check is kept |
| `TUR-W0373` | a nonlinear subterm was abstracted; arithmetic reasoning is incomplete for it |
| `TUR-W0060` | the `refined` experiment is in use (prototype lifecycle notice) |

`TUR-E0371` and `TUR-W0372` both leave the program safe -- the runtime check
survives in each case. Under `--strict-refine` both become hard errors.

Run `tur explain TUR-W0372` (or any of the codes above) for the long form.

---

## Debugging an obligation

Two environment variables:

```sh
TUR_REFINE_STATS=1 tur build --enable=refined main.tur
# refine: 3 obligation(s): 2 proven, 0 refuted, 1 unknown (7 backend call(s))

TUR_REFINE_DUMP=1 tur emit-c --enable=refined main.tur
# --- refinement VC (return value of double-pos) ---
# (set-logic QF_UFLIA)
# (declare-const x Int)
# (assert (< 0 x))
# (assert (not (< 0 (* x 2))))
# (check-sat)
```

The dump is SMT-LIB2 with the goal **negated** -- `unsat` means valid. It is
there to read, and to paste into an external solver when you want a second
opinion; the in-house stages consume the internal representation directly and
never go through this text.

### Cross-checking against Z3 (compiler developers)

The compiler ships no solver dependency, but a development build can link a
system Z3 as a correctness oracle: every obligation is decided by both, and a
disagreement is reported as `TUR-I0379` and downgraded to unknown so the build
stays sound. The option is off by default and Release and WASM builds refuse
it outright, so it cannot reach a shipped artifact.

```sh
# any Z3 >= 4.12 that provides a CMake package config
cmake -S . -B build-oracle -DCMAKE_BUILD_TYPE=Debug       -DTUR_REFINE_Z3_ORACLE=ON -DZ3_DIR=/path/to/lib/cmake/z3
cmake --build build-oracle -j
```

An oracle build also produces `tur_refine_fuzz`, which generates random
verification conditions and fails on any disagreement in either direction. It
is deterministic; a failure prints the seed that reproduces it. Expect roughly
13 VCs/second in a Debug build -- a fresh Z3 context per query is what costs,
and it is what keeps the oracle honest.

```sh
TUR_FUZZ_SEED=42 TUR_FUZZ_ITERS=20000 ./build-oracle/tur_refine_fuzz
```

Note that Z3's `sat` is only a counterexample when the VC contains no
uninterpreted symbols. Abstraction is sound in one direction only: `unsat` of
an abstracted VC implies the concrete obligation holds, but a model that
assigns an opaque symbol a convenient value proves nothing about the function
it stands for.

### Source-level differential fuzzing

`tur_refine_fuzz` starts at the VC, *below* the encoder, so it is blind to
every bug in the translation from Turmeric source into a VC. Both soundness
bugs found in this work lived exactly there, and neither was visible to it.

`tests/refine-fuzz-src.py` starts at the top instead. It generates whole
programs, compiles and runs each one with the gate off and on, and compares
what actually happened. It needs no Z3 -- the oracle is the gate-off build.

```sh
python3 tests/refine-fuzz-src.py --n 500 --seed 3
bash tests/run-refine-fuzz-src.sh          # smoke size, also ctest
```

The property it enforces is the one the design turns on:

> gate-off aborted on a contract  =>  gate-on must not run to completion

plus identical stdout when both run clean, and no gate-on abort on a program
that ran clean without it. A fourth bucket, `SUSPICIOUS_over_refute`, collects
programs that run clean with the gate off and are rejected with it on. Those
are usually *correct*: `TUR-E0371` is universally quantified over a function's
inputs while the program only exercises the arguments it happens to pass, so a
sound refutation and a clean run coexist routinely. They are counted and saved
for triage, never failed on.

To confirm the harness can fail, break purity on purpose (in
`rt_binding_is_pure`, return `true` unconditionally), rebuild, and rerun: a
sabotaged compiler reports `BUG_soundness` cases with minimal saved repros.
A fuzzer nobody has watched fail is a fuzzer that reports zero because it is
broken.

---

## Limits

Known and deliberate, in rough order of how likely you are to hit them:

- **A callee's entry check is never elided.** See above -- the call-site layer
  reports, it does not remove the callee's guard. Whole-program elision is a
  separate piece of work with real soundness preconditions.
- **Higher-order callees are not checked.** A function-typed parameter carries
  no refinements in its type, so `(defn apply2 [f : (fn [int int] int) ...])`
  cannot know what `f`'s arguments must satisfy, and neither can a call to
  `apply2`. Passing a refined function as a value is legal and the callee's own
  entry checks still run -- only the static crossing is lost. Refinements in
  function *types* are outside the prototype.
- **A dynamic typeclass dispatch is not checked.** When the instance is
  statically resolved the crossing is checked; when it is not, which method
  runs is unknown at the site, so only the method's own entry check applies.

- **Result-refinement propagation is order-dependent for a function's OWN
  return obligation.** Call-site crossings are resolved after the whole unit,
  so they always see every callee's refinement. A function's own return
  obligation is decided inline (that is what lets its check be elided), so it
  only sees refinements of functions already elaborated. Under mutual
  recursion, one direction may miss one. This can only lose a hypothesis, never
  add a false one.
- **No branching-body path sensitivity.** The return obligation is taken against
  the function's tail expression. A body whose tail is a `let`, a `match`, or a
  call lands outside the encoder's fragment and answers unknown.
- **No refinements on type parameters, typeclass method signatures, or
  higher-order predicates.** These are rejected or fall through to runtime.
- **Nonlinear arithmetic** is uninterpreted, as described above.
- **Purity is a syntactic whitelist, not an analysis.** A function whose body
  steps outside the admitted forms is impure even when it is in fact pure --
  a `match`, a struct field read, or a loop is enough. Its calls are then not
  congruent and measure-style reasoning over it does not go through. This
  costs completeness, never soundness. Widening the whitelist is the natural
  next increment; the effect row cannot substitute for it, because an empty
  row is not a purity claim.

Every one of these fails toward a runtime check, never toward a wrong answer.

---

## See also

- [contract-types-guide.md](contract-types-guide.md) -- the always-on runtime half
- [experimental-flags-guide.md](experimental-flags-guide.md) -- the `--enable=` mechanism
- [syntax-guide.md](syntax-guide.md) -- `#lang` layers
- [../upcoming/v1/refinement-types-plan.md](../upcoming/v1/refinement-types-plan.md) -- design, staging, and what is left
