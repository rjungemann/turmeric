# Float refinement proofs assume exact reals; the runtime check runs in double

**Severity:** medium -- a genuine miscompile class, but only for a float
refinement whose proof depends on an arithmetic identity that IEEE rounding
breaks. Every obligation still has its runtime fallback; the fallback is what
gets elided here.

## Summary

S2 (`refine_solver_arith.c`) decides real-sorted VCs over exact rationals.
The emitted contract check evaluates the same predicate in C `double`
arithmetic. Where the two disagree, a proof S2 finds is a proof about numbers
the program never computes, and the runtime check it elides is one that would
have fired.

## Minimal repro

```turmeric
;; Over exact reals (x + 0.1) - 0.1 = x, so S2 proves the return refinement
;; and the check is elided.  In doubles it is false for x = 0.3.
(defn g [x : #refine{ v : float | (> v 0.0) }] : #refine{ r : float | (= r x) }
  (- (+ x 0.1) 0.1))

(defn main [] : int
  (println (g 0.3))
  0)
```

| build | result |
|---|---|
| `tur build` | `2 obligation(s): 2 proven`; prints `0.3`, exit 0 |
| `TUR_REFINE_NO_DISCHARGE=1 tur build` | `panic ...: Return contract violated` |

(0.3 + 0.1) - 0.1 is 0.30000000000000004 in double, and `=` on doubles is
exact. Strict inequalities near a boundary fail the same way
(`x < 1.0 |= x + 0.1 < 1.1` is provable and false for the largest double
below 1.0), and so does anything that can underflow
(`x > 0.0 |= x * 0.25 > 0.0` fails for the smallest subnormal).

## Root cause

`linearize` (`refine_solver_arith.c`) treats `+ - *` over real-sorted terms
as exact field operations, and `vc_mk` (`refine_vc.c`) folds real literals
with `double` arithmetic and then reasons about the result as a rational.
Nothing in the pipeline records that a real-sorted value is a `double`. The
in-house model search, by contrast, now evaluates reals in `double`, so a
counterexample it reports is one the program would reject; it is the
proving side that is optimistic.

This is not a bug in any one stage: it is the choice, never written down
until now, to give `float` the semantics of `Real`. Several refinement-type
systems make the same choice and carry the same caveat. The user guide now
states it (`docs/guides/refinement-types-guide.md`, "Floats are proved as
exact reals").

## Fix directions

1. **Keep the choice, keep the caveat** (the state after this note). Cheapest;
   float refinements stay useful for the common monotone shapes
   (`x >= 0.0 |= x * 0.25 >= 0.0`, `x >= 0.0 |= x + 7.1 > 0.0`), which are
   also true in double.
2. **A rounding-safe fragment.** Let S2/S3 decide a real-sorted cube only
   when its refutation needs no real *arithmetic*: comparisons between
   real variables and literals (transitivity, bounds) are exact in double
   because no operation rounds. A cube with an `ADD`/`SUB`/`MUL`/`DIV`/`NEG`
   node of sort `VS_REAL` answers `RT_UNKNOWN`. Sound, one predicate in
   `la_assert_cube` / `euf_assert_cube`, and it regresses
   `tests/fixtures/refine-float-lra` (both of its proofs use arithmetic)
   and `refine-float-measure` -- those would need `--strict-refine` removed
   or the expectations changed.
3. **Model the rounding.** Widen each real operation's result by a relative
   error bound (`fl(a op b)` lies in `(a op b)(1 +/- 2^-53)`), which turns an
   equality goal over reals into two inequalities with slack and keeps the
   monotone proofs. Correct and complete for the common cases, but a real
   project (interval reasoning inside Fourier-Motzkin), and it does not
   cover underflow, overflow, or NaN.

Direction 2 is the one consistent with the guide's stated invariant ("turning
`refined` on can never turn a correct program into a wrong one"); direction 1
is the one consistent with the existing fixtures. The choice belongs to the
maintainer, which is why this is a report and not a patch.
