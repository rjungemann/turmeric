# A definite refinement violation goes unreported when the caller has any parameter

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

## Found

While landing the class-signature crossing for dynamic typeclass dispatch
(`tests/fixtures/errors/refine-class-param-dynamic-violated`). That fixture has
to pin its violation under `--strict-refine` for this reason: the caller of a
dynamic dispatch necessarily has a parameter (the abstract receiver), so its
model is never "closed" under the current test. The defect is independent of
typeclasses -- the repro above uses a plain `defn`.
