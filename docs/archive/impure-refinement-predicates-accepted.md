# Impure refinement/contract predicates are accepted; TUR-E0375 never fires

**Status:** RESOLVED 2026-07-25.
**Severity when open:** medium (behaviour differed between builds; not a miscompile)

## The defect

A `#refine{...}` / `:pre` / `:post` predicate could call an effectful function
and nothing rejected it. `TUR_E0375_REFINE_EFFECTFUL` ("refinement predicate
mentions effects; pure predicates only") was declared in `diag.h:242` and
mapped in `diag.c:247`/`:394`, and **emitted by nothing** -- grep returned only
the three declaration lines.

Whether a check runs is a property of the BUILD: `--no-contracts` strips every
check, a release build drops them unless `--keep-contracts`, and
`--enable=refined` elides the ones it can prove. A predicate with side effects
therefore made program behaviour depend on whether its own contracts were
compiled in.

## Repro (now an error)

```turmeric
(defn tick [] #fx{} : int
  ```c
  static int64_t n = 0;
  return n++;
  ```)

(defn target [] : #refine{ r : int | (>= (tick) 0) }
  (tick))
```

Before: default build printed `0, 2`; `--no-contracts` printed `0, 1`.
Now: `error [TUR-E0375]: contract predicate has side effects; predicates must
be pure`.

## How it surfaced

`tests/refine-fuzz-src.py` reported it as `BUG_output_divergence` (seed 93,
case 294), not as a soundness bug -- both runs were internally consistent, they
just printed different numbers. The generated predicate called an impure helper
and contained a tautology (`(not= r (+ 2 r))`), so the obligation discharged,
the check was elided, and eliding it skipped the predicate's own side effects.

## The fix, and the design point it forced

`TUR-E0375` is now emitted from the shared CT1 helpers, covering all four
positions a predicate can appear in: a refined parameter, `:pre`, `:post`, and
a refined return. Gate-independent -- the predicate is equally wrong with the
refinement experiment off.

The interesting part is what it could NOT be driven by. The existing purity
test (`rt_binding_is_pure`) is DEFAULT-DENY: anything the walk does not model
-- a `match`, a struct field read -- reads as impure. That is right for
congruence, where a wrong "pure" elides a real check. It is exactly wrong for a
diagnostic, where a wrong "impure" rejects working code: a measure written with
a `match` would have been reported as effectful.

So purity is now a THREE-valued classification -- `RT_P_PURE` / `RT_P_IMPURE` /
`RT_P_UNKNOWN` -- and the two callers read the middle value in opposite
directions:

| question | wrong answer costs | reads UNKNOWN as |
|---|---|---|
| congruence: same value twice? | an elided check (miscompile) | impure |
| diagnostic: does this DO something? | a rejected valid program | pure |

They are not negations of each other, and collapsing them into one boolean is
how a default-deny purity test turns into false errors.

## Verification

- `errors/refine-impure-predicate` -- all four predicate positions.
- `refine-pure-predicate-unmodelled` -- a `match`-bodied measure in a `:post`,
  which must still compile. This is the false-positive guard.
- Full suite: a hard error broke nothing that existed. Across 2308 fixtures and
  the whole stdlib, the only impure predicate was the one written to
  demonstrate the bug -- which is why the declared severity (an E-code, not a
  warning) turned out to be affordable.

## Follow-on noted during the fix

The fuzzer's generated predicates were calling impure helpers ~8% of the time,
so those cases became `skip_invalid` and wasted budget rediscovering a rule one
fixture already pins. `Gen.expr` grew a `pure_only` flag used by predicate
generation; `skip_invalid` went from ~24/300 to ~2/300.
