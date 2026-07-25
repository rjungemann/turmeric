# A definite refinement violation goes unreported when the caller has any parameter (RESOLVED)

**Severity:** medium -- a compile-time error that should fire does not. No
soundness implication: the obligation is runtime-guarded, so the callee's entry
check still catches the violation at runtime. What is lost is catching it at
compile time, in exactly the case the feature advertises (`(safe-div 10 0)`
becoming a compile-time failure instead of a runtime panic).

## Repro

The two calls are identical. Only the caller's signature differs.

```turmeric
(defn safe-div [a : int b : #refine{ v : int | (not= v 0) }] : int (/ a b))

(defn c-noparam [] : int      (safe-div 10 0))   ; error[TUR-E0371]  <- reported
(defn c-param   [n : int] : int (safe-div 10 0)) ; silent            <- not reported

(defn main [] : int 0)
```

```
$ ./build/tur check --enable=refined repro.tur
repro.tur:3:26: error [TUR-E0371]: refinement on argument 2 of 'safe-div' in 'c-noparam' ...
```

`c-param` is reported only under `--strict-refine`, where every unproven
obligation errors anyway. The unrelated parameter `n` is never mentioned by the
goal.

## Root cause

`src/compiler/refine_discharge.c`, the `RT_INVALID` case:

```c
bool closed = !d.model || d.model->n == 0;
if (ob->runtime_guarded && !closed && !g_strict_refine) {
    g_stats.unknown++;
    return false;
}
```

The intent is right: for a runtime-guarded obligation an OPEN counterexample
only says "not for every input", which is not itself a defect, while a CLOSED
one means the goal folded to false on the values written at the site.

The test for closedness is the problem. `d.model->n` counts the variables the
VC declared, which includes every variable in scope at the crossing -- the
caller's parameters -- whether or not the goal mentions them. So "closed" is
really "the caller has no parameters in scope", and any parameterised caller
loses definite-violation reporting even for a fully literal argument list.

The counterexample text shows it: the model printed for the typeclass case is
`x = -2`, binding the receiver, while the goal is `(> 0 0)` -- false for every
`x`.

## Fix direction

Judge closedness by whether the GOAL (after substitution) mentions any free
variable, rather than by the model's size. A variable-free goal that evaluates
false is unconditionally violated, and the model search has already witnessed
that the hypotheses are satisfiable, so the path is not dead.

Two things to be careful about:

- Keep the hypothesis check. The obligation is `hyps |- goal`; the argument
  above holds because `refine_model_search` returns an assignment satisfying
  `hyps AND (not goal)`, so it may not be weakened to "goal is false" alone.
- This widens the set of programs that get a hard error by default, across
  every crossing rather than any one construct. It is a diagnostic behaviour
  change and deserves a suite run and a look at what newly errors before it
  lands, not a drive-by fix.

## Resolution

Closedness is now read off the GOAL rather than the model. `vc_term_is_ground`
walks the (already substituted) goal term; a goal mentioning no variable is
closed no matter what else the VC declared.

The fix is monotone -- the old test is still accepted, the ground-goal case is
added -- so it can only widen what is reported, never narrow it. The hypothesis
half is untouched and load-bearing: `refine_model_search` still has to satisfy
the hypotheses, which is what keeps the widened rule from firing on a branch
the path conditions exclude (`n > 0 AND n < 0` has no model, so the
`(safe-div 10 0)` inside it stays unreported).

A second, smaller wart went with it. `emit_model_note` suppressed the
counterexample line in the closed case -- correctly, since the one-line
predicate note already says it -- but decided that from the model's size too,
so the newly-reported cases printed `counterexample: n = -2` for a goal false
regardless of `n`. It now takes the same `closed` flag as `emit_predicate_note`.

### What it changed, measured

Less than the report feared. The suite went 2336 -> 2338 with two added
fixtures and **zero pre-existing fixtures newly erroring**, and the source-level
fuzzer returned classification counts identical to their recorded baselines
across four seeds and 800 cases (suspicious 4/5/4/13, 0 soundness bugs, 0 other
BUG classes). The shape it targets is simply not one that working code
contains -- which is the point, since a definite violation is a bug.

It did strengthen the preceding slice for free: a violating argument at a
dynamic typeclass dispatch now reports by DEFAULT rather than only under
`--strict-refine`, because the abstract receiver was exactly the "unrelated
parameter in scope" that made those models look open.
`tests/fixtures/errors/refine-class-param-dynamic-violated` was moved off
`--strict-refine` to pin that.

### The accepted cost

A violating call on a reachable-but-not-exercised branch is now reported:

```turmeric
(defn f [n : int] : int (if (> n 0) 1 (safe-div 10 0)))
(defn main [] : int (println (f 5)) 0)   ; never takes the else branch
```

Gate-off runs clean; gate-on errors. That is a latent bug -- `(f 0)` aborts --
and reporting it is the same standard already applied to a zero-parameter
caller, so the change makes the treatment consistent rather than introducing a
new posture. No fuzz case hit this shape.

## Coverage

- `tests/fixtures/errors/refine-definite-violation-param-caller` -- the report's
  two callers, plus a constant-folded argument.
- `tests/fixtures/refine-open-goal-not-reported` -- the half that matters more:
  argument-is-a-parameter, derived-from-a-parameter, discharged-by-hypothesis,
  definite-violation-on-an-excluded-path, and a satisfying literal. All compile
  clean.

## Found

While landing the class-signature crossing for dynamic typeclass dispatch
(`tests/fixtures/errors/refine-class-param-dynamic-violated`). That fixture has
to pin its violation under `--strict-refine` for this reason: the caller of a
dynamic dispatch necessarily has a parameter (the abstract receiver), so its
model is never "closed" under the current test. The defect is independent of
typeclasses -- the repro above uses a plain `defn`.
