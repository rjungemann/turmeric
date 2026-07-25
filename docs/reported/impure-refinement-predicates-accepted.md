# Impure refinement/contract predicates are accepted; TUR-E0375 never fires

**Severity:** medium (behaviour differs between builds; not a miscompile)

A `#refine{...}` / `:pre` / `:post` predicate may call an effectful function,
and nothing rejects it. `TUR-E0375_REFINE_EFFECTFUL` ("refinement predicate
mentions effects; pure predicates only") is declared in `diag.h:242` and mapped
in `diag.c:247`/`:394`, but **no site emits it** -- grep returns only the three
declaration lines.

## Repro

```turmeric
(defn tick [] #fx{} : int
  ```c
  static int64_t n = 0;
  return n++;
  ```)

(defn target [] : #refine{ r : int | (>= (tick) 0) }
  (tick))

(defn main [] : int
  (println (target))
  (println (target))
  0)
```

Evaluating the contract advances the counter, so the values the program prints
depend on whether the check is emitted:

- default build: `0`, `2` (the check consumes a counter value each call)
- `--no-contracts`: `0`, `1`

That divergence is a property of the CONTRACT layer and predates refinement
types -- `--no-contracts` is documented to strip checks, and a predicate with
side effects makes stripping observable.

## How it surfaced

`tests/refine-fuzz-src.py` reported it as `BUG_output_divergence` (seed 93,
case 294), not as a soundness bug: both runs were internally consistent, they
just printed different numbers. The generated predicate called an impure helper
and contained a tautology (`(not= r (+ 2 r))`), so the obligation discharged and
the check was elided -- with the same effect as `--no-contracts`, but reached by
turning an experiment on.

## What has been fixed

Only the part the refinement work owns: `rt_pred_is_impure` (`elab_fns.c`) now
refuses to report a return obligation as proven when the predicate calls
anything not known pure, so `--enable=refined` never elides an observable
check. Turning the gate on cannot change behaviour. Fixture:
`refine-impure-predicate-not-elided`.

## What remains

The predicate should be rejected, or at least warned about, wherever contracts
are elaborated -- independent of the gate, since `--no-contracts` shows the same
divergence with the experiment off. `rt_pred_is_impure` is the test to reuse;
the open questions are which diagnostic (E0375 as a hard error would reject
programs that compile today) and whether `:pre`/`:post` get the same treatment.

## Fix directions

- Emit `TUR-E0375` from CT1 predicate elaboration, gated behind
  `--strict-refine` initially so it does not break existing code.
- Or make it a warning unconditionally and an error under `--strict-refine`.
