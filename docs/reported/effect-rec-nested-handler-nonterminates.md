---
status: open
severity: high
discovered: 2026-07-24
area: runtime (DK-lowered effects / nested handler resume)
---

# Resuming an outer effect through an inner `handle` loop never terminates

## Summary

On the current DK-lowered effect runtime, resuming an **outer** effect whose
captured continuation re-enters an **inner** `handle` running its own
perform/resume loop **does not terminate** -- the program loops forever (verified
hanging even at loop depth `N = 10`, not merely slow at large `N`).

This is the bug the `effect-rec` sign-off probe
(`tests/probes/stackless-signoff/effect-rec.tur`) was written to guard, but the
probe as committed had no timeout, so instead of failing it hung the entire
`stackless_signoff_probes` ctest target -- which is what stalled the Ubuntu /
macOS CI job until GitHub canceled it (~32 min). The probe is now quarantined as
an `xfail` under a hard per-probe timeout (see `tests/stackless-signoff-probes.sh`);
this report tracks the underlying runtime defect. Remove `effect-rec` from the
`xfail` set in that script once the fix lands (it will XPASS and re-arm).

## Minimal repro

The full probe drives 1,000,000 iterations, but the non-termination reproduces
with any depth. Reduced to `N = 3`:

```turmeric
(defeffect Outer [] :int)
(defeffect Tick [] :int)

(defn tick-loop [n : int acc : int] : int
  (if (= n 0) acc (tick-loop (- n 1) (+ acc (perform (Tick))))))

(defn run-ticks [n : int] : int
  (handle (tick-loop n 0)
    (Tick [] k) (resume k 1)))

(defn main [] : int
  (println
    (handle (+ (perform (Outer)) (run-ticks 3))   ; expect 3
      (Outer [] k) (resume k 0)))
  0)
```

```sh
tur build repro.tur -o /tmp/repro && /tmp/repro   # hangs; never prints
```

## What isolates the bug -- all three elements are required

Each of these variants terminates correctly; only their combination hangs:

- **Tick loop alone** (`run-ticks 3`, no outer handler) -> prints `3`. OK.
- **Outer handle wrapping the Tick loop but no `(perform (Outer))`** -> `3`. OK.
- **Outer perform/resume around a plain value** (`(+ (perform (Outer)) 42)`,
  no inner `handle`) -> `42`. OK.
- **Outer perform/resume whose continuation contains the inner `handle` loop**
  -> **hangs.**

So the trigger is specifically: *resuming an outer effect whose delimited
continuation re-enters an inner `handle` that itself performs and resumes.*

## Root cause -- direction

Effects are DK/CPS-lowered (the fiber-based effect runtime the probe's header
comment describes is stale; `cps-effects` graduated -- see
`src/runtime/experiments.c:160`). The suspect is the reify/re-install path in
`dk_perform` (`src/runtime/cps_prompt.c:258`): for a deep handler it captures
`dk_copy_range(k, H)` and appends a re-installed handler
(`src/runtime/cps_prompt.c:283-287`). When the resumed outer continuation
carries an inner handler marker, the handler search
(`src/runtime/cps_prompt.c:260-262`) and/or the deep re-install appears to keep
re-delimiting under the same marker so the inner loop never makes progress.
Needs a walk of the DK chain at the outer `resume` to confirm whether the inner
handler marker is being duplicated/re-entered rather than advanced.

## Impact

Nested handlers (an outer effect resumed across an inner effectful computation)
are a normal, expected composition. Any program of that shape hangs. Fixing this
is v1 effect-runtime work; the probe stands as the regression guard.
