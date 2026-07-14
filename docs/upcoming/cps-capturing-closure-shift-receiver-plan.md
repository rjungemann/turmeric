---
title: "CPS backend -- native emission of capturing-closure shift receivers (resuming shift, N6.3/N6-Task1)"
status: proposed
description: A `shift` whose receiver is a CAPTURING closure that resumes the captured continuation (e.g. `(shift (fn [k] (k base)) 0)`, capturing `base`) is SIG/BODY-evicted with `EX_CLOSURE (capturing closure)` and falls back to the direct emitter's fiber shift lowering, where it leaks. Non-capturing resuming receivers (`(fn [k] (k 1))`, incl. multi-shot `(+ (k 1) (k 2))`) are ALREADY native via the `perform __Shift` desugar. This plan closes the remaining capturing case by binding `subk` and lifting the receiver body with its captures, instead of synthesizing an indirect `(recv val)` call that a fat closure cannot service.
---

## Symptom

`tests/fixtures/shift-crossfn-resume-works` -- `step` evicts:

```
cps-fn step [base] k:cont<int> entry
  <unsupported: unsupported form: EX_CLOSURE (capturing closure)>
```

`(defn step [base] (shift (fn [k : cont] (k base)) 0))` -- the shift receiver
`(fn [k] (k base))` captures `base` and resumes `k`. It falls back to the direct
emitter and leaks under ASan.

## What already works (scope boundary)

In the SAME fixture, these are native (they show `perform __Shift(...)` in
`--dump-cps`, not `<unsupported>`):

- `inner`  = `(shift (fn [k] (k 1)) 0)`            -- resuming, **no capture**
- `twice`  = `(shift (fn [k] (+ (k 1) (k 2))) 0)`  -- **multi-shot**, no capture
- `thrice` = `(shift (fn [k] (+ (+ (k 1) (k 2)) (k 3))) 0)` -- multi-shot, no capture

So resuming AND multi-shot receivers are already lowered natively via the base
shift's synthetic `__Shift` effect (perform/resume desugar). The remaining gap is
exactly **capture** in the receiver.

## Root cause -- CORRECTED after starting execution (my original model was wrong)

The original model below (a `(recv val)` synthesis / beta-reduce in
`cps_shift_body_kf`) is **wrong**: instrumentation shows `cps_shift_body_kf` is
NEVER called for this fixture -- it is dead for the base cross-function shift path.

The real mechanism is the `__Shift` effect desugar in
`src/compiler/elab_effects.c`: a base cross-function shift lowers as
`(shift RECV BODY) -> (perform (__Shift RECV))` (:745), and a reset body is
wrapped `B -> (handle B (__Shift [recv] k) (recv k))` (:560) so the handler
applies the receiver to the captured continuation. So the receiver `RECV` crosses
as the **`__Shift` perform ARGUMENT**, and is applied `(recv k)` inside the
handler in the RESET function (cross-function).

`--dump-cps` confirms:
- `inner` (`(fn [k] (k 1))`, non-capturing): `let __t1 = <value>; perform __Shift(__t1)`
  -- the receiver is lifted to a delegated value (`is_delegatable_value`,
  cps_ir.c:377-388, admits a closure with `n_captures == 0`).
- `step` (`(fn [k] (k base))`, captures `base`): the whole body is
  `<unsupported: EX_CLOSURE (capturing closure)>` -- `is_delegatable_value`
  rejects `n_captures > 0` (the capture cut can't see its free vars), so the
  capturing receiver hits `build_unsupported`.

So the gap is: **a capturing-closure receiver passed as the `__Shift` perform
argument** (a fat closure crossing the effect ABI, applied `(recv k)` in the
handler cross-function), NOT anything in `cps_shift_body_kf` / `emit_shift`.

## Approach (REVISED)

Admit a capturing-closure `__Shift` receiver as a delegated fat-closure value:
construct it (env carrying `base`) via the direct emitter's closure path
(CT_LETRAW), surfacing its captured free vars to the has_capture cut so they are
seen, and pass the fat-closure value as the `perform __Shift` argument. The
handler side `(recv k)` is already direct-emitted in the reset function and
applies a fat closure normally.

- The change is in `cps_ir.c`'s delegation / capture-cut logic
  (`is_delegatable_value` + `has_capture` surfacing), NOT `emit_shift`.
- The captured free vars (`base`) must ride the fat-closure env constructed at the
  shift site and travel with the closure value across the `perform __Shift`
  boundary into the handler; this is cross-function fat-closure env marshaling
  through the effect argument.

### Increased scope / risk vs the original plan

This is larger and subtler than the original write-up: it is fat-closure-as-effect
-payload plus indirect fat-closure application, with a cross-function env lifetime,
gated by the has_capture cut. Miscompile risk (wrong captured value, env lifetime)
is real. Proceed only with the direct==cps oracle (11/105/23/306) green AND an
ASan pass; if the capture-cut interaction is not cleanly expressible, re-scope
rather than force it.

3. **Leaks.** Any Tier-C boxes crossing on this path go through `slot_store_reap`
   already (from the Tier-C-effect-result work); confirm the capturing path is
   covered. NOTE: `shift-crossfn-resume-works`'s 14-box ASan leak is NOT solely
   from `step`'s eviction -- the native multi-shot resume path (`twice`/`thrice`)
   also allocates per-resume nodes (`dk_frame_resume`, see the separate
   `two-perform` finding) that leak independently. Closing this eviction removes
   `step`'s fiber-fallback boxes; the multi-shot resume-node leak is tracked
   separately and may keep the fixture's marker until both are done.

## Verification

- Oracle: `shift-crossfn-resume-works` already asserts `direct == cps` values
  (`11`, `105`, `23`, `306`). `step` becoming native must keep all four exact.
- `--dump-cps` for `step` shows `perform __Shift` (no `<unsupported>`), and the
  emitted `step` is `step__cps` (not the direct `tur_effect_*` fallback).
- ASan: no double-free / use-after-free; `step`'s fiber-fallback boxes gone.
- Full suite green; snapshots regenerated.

## Risks

- Control-flow lowering on the delimited-control surface -- miscompile risk is
  real. Mitigated by the existing direct==cps oracle (single- AND multi-shot) and
  an ASan pass; do not land unless all four values match and the sweep is clean.
- Capturing a NON-scalar (owning) free var in the receiver is out of scope here
  (it bails to the fallback via the existing `collect_caps` non-Copy guard, same
  as the abortive path) -- this plan covers scalar/Copy captures only.

## Non-goals

- Non-capturing resuming / multi-shot receivers -- already native.
- The `dk_frame_resume` per-resume node leak (multi-shot) -- separate finding.
- Owning-field captures in the receiver -- gate item 4 / env-capture-owning-values.
- Deleting the general fallback (N6.5 / Task 2) -- this is one of its inputs.
