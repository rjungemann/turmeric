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

## Root cause

`src/passes/cps_ir.c:indirect_callee_ok` (:402) rejects a capturing closure as an
indirect callee: the base-shift lowering synthesizes `(recv val)` -- applying the
receiver VALUE to the reified continuation -- which the direct emitter's indirect
call path emits by casting the callee value to a bare fn pointer. A fat (capturing)
closure's value is its ENV pointer, so that cast would jump into the heap env
struct as code. To stay sound the classifier evicts (comment at :396-400, which
still cites the now-deleted `emit_cps.c` as the fallback owner -- stale).

The abortive shift path (`emit_shift`, `emit_cps_ir.c`) already lifts the shift
BODY with its captures (`collect_caps` + `emit_lifted(LH_SHIFT_BODY, caps)`), so
the capture-carrying mechanism exists; it is only the *resuming* receiver that is
forced through the `(recv val)` indirect-call synthesis and thus evicts.

## Approach (proposed)

Do not synthesize an indirect `(recv val)` for a capturing resuming receiver.
Instead lower it the way the non-capturing resuming receivers already lower --
the `__Shift` perform/resume desugar -- but carry the receiver's captures:

1. **Classifier (`cps_ir.c`).** When the shift receiver is an `EX_CLOSURE` with
   captures AND references its continuation param, admit it to the `__Shift`
   desugar with the captured free vars surfaced (as the abortive path already does
   via `collect_caps`), rather than routing to the `(recv val)` indirect path that
   `indirect_callee_ok` rejects. `(k v)` in the body is the existing continuation
   resume (already representable -- that is how `inner`/`twice`/`thrice` lower).
2. **Emit (`emit_cps_ir.c`).** Reuse the receiver-body lift that carries captures
   in the lifted env (the `LH_SHIFT_BODY` env struct); `(k v)` lowers to the same
   resume the non-capturing case emits. No new indirect call. The captured `base`
   rides the env, `k` is `subk`.
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
