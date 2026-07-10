# Direct backend degrades out-of-subset `reset`/`shift` to plain body-eval

**Severity:** medium (silent wrong value on the default backend; the value
differs from the documented abortive-shift semantics).

## Summary

On the **default** backend, a `(reset ...)` whose body reaches a base `shift`
through a shape outside `emit_cps.c`'s admitted subset does not implement
delimited control at all -- `emit_effects_reset` falls back to
`emit_value(body)` (just evaluate the body), and the out-of-context `shift`
lowers to "return its operand value." The abortive continuation is therefore
*not* discarded, so the reset yields a different value than real shift/reset
semantics.

## Minimal repro

```turmeric
(defn f [c : bool] : int
  (reset (+ 10 (if c (shift (fn [v] v) 1) 5))))

(defn main [] : int
  (println (f true))    ;; prints 11 on the default backend
  0)
```

- **Default backend:** `11` -- the reset degrades to body-eval, the `shift`
  returns its operand `1`, and `(+ 10 1) = 11`.
- **Correct (abortive) semantics:** `1` -- the `shift` is abortive; it discards
  the `(+ 10 [])` delimited continuation, so the reset yields the shift result.

The intended semantics is abortive: the shipped fixture `cps-backend-shift0`
asserts `(reset (+ 10 (shift0 (fn [v : int] v) 5)))` = `5` (not `15`), i.e. the
`+ 10` continuation is discarded. So `11` above is a degraded/wrong result.

## Root cause

- `src/compiler/emit_effects.c:1209` `emit_effects_reset` -> calls
  `emit_cps_reset` (`src/compiler/emit_cps.c:336`); outside that function's
  subset it returns NULL and the caller falls back to `emit_value(body)` --
  reset becomes a no-op boundary.
- The `shift` inside then lowers via `emit_effects_shift`
  (`src/compiler/emit_effects.c:1437`) with no enclosing CPS prompt, delivering
  its operand as an ordinary value instead of aborting.

## Interaction with the CPS backend unification (U1)

The CT-IR backend (`--enable=cps-backend`) *does* implement abortive semantics
for these shapes. The U1 slice of `cps-backend-unification-plan.md` broadened
the CT-IR admission (`delim_ok` in `src/compiler/emit_cps_ir.c`) so that
reset bodies with control flow around a **tail-position** shift lower onto DK.
It deliberately **excludes** the join-bearing shapes above (a `letcont` whose
jbody delivers to the prompt) precisely because the CPS path would then produce
the correct `1` while the default path still degrades to `11` -- breaking the
migration's `direct == cps` invariant.

Closing this report (teaching the direct/`emit_cps.c` path the correct abortive
lowering for these join-bearing shapes, or retiring the direct path per U7) is
what would let U1 admit the excluded shapes without a backend-divergence.

## Fix directions

- Preferred long-term: U7 of the unification retires `emit_cps.c`, making the
  CT-IR backend the sole (correct) lowering -- the degradation disappears.
- Shorter-term: extend `emit_cps_reset`'s subset (or `emit_effects_reset`'s
  fallback) so an out-of-subset reset with a reachable shift still aborts the
  delimited continuation rather than evaluating it.
