---
title: Resumable delimited control -- one substrate (k-reset / k-shift)
category: Planning
status: in progress -- slice 1 LANDED (k-reset/k-shift aliased onto the cloneable pipeline, multi-shot, resumed via tur_cloneable_cont_resume). Open: (k v) resume sugar, single-shot specialization, retiring abortive shift + cloneable/serial into this surface.
description: Split out of cps-backend-n6-fallback-removal-followups-plan.md (Task 1). Rather than reinterpret the abortive `shift` (breaking ~36 fixtures + the interpreter), add a NEW resumable delimited-control surface (`k-reset` / `k-shift`) that lowers to the SAME substrate the cloneable/serial variants already use -- the DK machine in compiled code, the reified-context TuriCont in the interpreter. Abortive `shift`/`shift0`, `cloneable-*`, and `serial-*` are then capability-specializations that retire into this one primitive over time.
---

# Resumable delimited control -- one substrate

## Direction (decided)

The abortive `shift` is not made resumable in place -- that would reinterpret
every existing `(shift (fn [v] ...) body)` site (~36 fixtures + the turi
interpreter tests) and change core semantics. Instead we add a **new resumable
surface** and, over time, retire the older forms into it.

Guiding realization (the whole reason this is one plan, not four): resumable
delimited control is **already one substrate**. In compiled code the DK machine
(`dk_shift` / `dk_invoke` / `dk_copy_range`, `src/compiler/emit_dk_runtime.c`)
backs abortive shift, cloneable, and serial alike -- the differences are
*properties of the captured continuation* (single-shot vs cloneable/multi-shot
vs marshalable), not different mechanisms. In the interpreter the reified-context
`TuriCont` / `TsFrame` capture (`src/turi/eval.c:1601+`) already backs both
cloneable and serial. The new surface reuses both; it does not add a fifth
machine.

## The new surface -- `k-reset` / `k-shift`

`(k-reset BODY)` delimits; `(k-shift receiver body)` captures the delimited
continuation `k` and hands it to `receiver`, a `(fn [k] ...)` that resumes `k`.
This is the standard shift/reset -- the receiver gets the continuation, NOT the
body value (which is what distinguishes it from abortive `shift`). Spelled with a
`k-` prefix so it coexists with the abortive `shift`/`reset` until those retire.

### As it works TODAY (slice 1, cloneable-backed)

Slice 1 aliases `k-reset`/`k-shift` onto the cloneable pipeline verbatim, so the
current surface is exactly cloneable's: the continuation is a **multi-shot**
cloneable handle, resumed with the explicit `tur_cloneable_cont_resume` /
`tur_cloneable_cont_clone` primitives -- **not** `(k v)` application sugar (that
sugar does not exist yet; `(k 1)` errors with "not a function or continuation").

```turmeric
(defn step [k] : int                                   ; k : cloneable-cont handle
  (+ (tur_cloneable_cont_resume (tur_cloneable_cont_clone k) 1)  ; resume once
     (tur_cloneable_cont_resume k 2)))                 ; resume again (multi-shot)

(k-reset (+ 10 (k-shift step 0)))   ; context (+ 10 []): (10+1) + (10+2) = 23
```

Inherited cloneable limitations apply today: only the native `build_cloneable`
context subset is supported (arithmetic binops, calls, pure lets, one `if`
branch point); a context outside it is rejected with `TUR-E0710`, not
miscompiled. The 2nd operand (`0` above) is the cloneable-shift seed/hole slot.

### The intended surface (follow-on slices)

- **`(k v)` resume sugar.** Make the captured continuation directly applicable --
  `(k 1)` instead of `(tur_cloneable_cont_resume k 1)` -- so `step` reads as
  `(+ (k 1) (k 2))`. This is elaboration sugar over the resume primitive.
- **Single-shot `k-shift`.** A lighter variant whose continuation is single-shot
  (plain `dk_shift`/`dk_invoke`, no `dk_copy_range` clone glue); resuming twice
  is a runtime error. Multi-shot stays available as the cloneable capability.
- **Typed continuation.** `receiver : (-> Cont<BodyT,ResetT> ResetT)` rather than
  the bare int handle the cloneable alias exposes today.

## Work plan (incremental, each slice testable)

**Slice 1 -- LANDED.** `k-reset` / `k-shift` recognized as canonical resumable
delimited control, aliased onto the cloneable pipeline (the proven shared
substrate). Because the alias reuses the whole cloneable path, the entire
lowering came in one move -- **no new pipeline code**:

- Symbols `sym_k_reset` / `sym_k_shift` (`elab_internal.h`, `elab_core.c`);
  dispatch to `elab_cloneable_reset` / `elab_cloneable_shift` (`elab_call.c`).
- Type-checking, direct emission on `dk_shift`/`dk_invoke`, and the CT-IR
  lowering are all inherited from the cloneable pipeline unchanged.
- Fixture `k-shift-resumable-basic`: `k-shift` receiver resumes `k` (twice,
  cloneable); `direct == cps == turi == 23 / 6`.

The trade-off this bought: the surface is exactly cloneable's today (multi-shot,
explicit resume primitive, `TUR-E0710` context subset). Slices 2-4 make it
ergonomic and add the single-shot/typed variants.

**Slice 2 -- OPEN -- `(k v)` resume sugar.** Elaborate an application of a
captured continuation `(k v)` to the resume primitive, so a `k-shift` receiver
reads as `(fn [k] (+ (k 1) (k 2)))`. Today `(k 1)` errors. Reuse the escape/call
application-lowering path (`elab_call.c`) that already special-cases continuation
application for `call/cc`. Done when the sugar and the explicit
`tur_cloneable_cont_resume` form both compile to the same result and
`k-shift-resumable-basic` can be rewritten with `(k v)`.

**Slice 3 -- OPEN -- single-shot `k-shift`.** A variant whose continuation is
single-shot: plain `dk_shift`/`dk_invoke`, no `dk_copy_range` clone glue;
resuming a second time is a runtime error (not silent). Lighter than the
cloneable alias. Done when a single-shot fixture (`direct == cps == turi`) works
and a double-resume yields a clean error on all three paths, LeakSanitizer-clean.

**Slice 4 -- OPEN -- typed continuation.** Expose `k` as
`Cont<BodyT,ResetT>` instead of the bare int handle the cloneable alias yields,
so the receiver signature is checkable. Done when a mismatched resume value is a
compile error and the handle is no longer an untyped `:int`.

## Consolidation (the retirement path -- OPEN, larger)

Once slices 2-4 land and `k-reset`/`k-shift` are the ergonomic primitive:

- **Abortive `shift`** becomes sugar: an abortive shift is a `k-shift` whose
  receiver ignores `k` and whose value aborts the prompt. Steps: (a) re-express
  `elab_shift`/`elab_shift0` in terms of the k-shift path, (b) migrate the ~36
  abortive fixtures + the turi tests, (c) delete the abortive-specific interp
  (`eval_abortive_shift`) and emit (`emit_effects_shift`) paths. Done when no
  abortive-specific lowering remains and the suite is green.
- **cloneable-shift / serial-shift** become `k-shift` with a *continuation
  capability* (cloneable = multi-shot clone; serial = marshalable). The capture
  machinery is already shared; only the capability annotation differs. Collapse
  the three receiver-lowering paths into one. Done when `cloneable-*`/`serial-*`
  are thin capability annotations over the k-shift path, not separate pipelines.

The end state is a single delimited-control primitive with a capability knob on
the continuation -- the "one substrate" this plan is named for. Each retirement
step is independently landable and suite-gated; none is a prerequisite for the
others.

## Where this came from

Task 1 of
[cps-backend-n6-fallback-removal-followups-plan.md](../../archive/cps-backend-n6-fallback-removal-followups-plan.md)
(archived). Analysis of why the in-place reinterpretation is a breaking language
change:
[cps-backend-n6-fallback-followups-blocked.md](../../reported/cps-backend-n6-fallback-followups-blocked.md).
Prior art on the abortive limitation:
[compiled-first-class-continuations-plan.md](../../archive/compiled-first-class-continuations-plan.md).

## Out of scope

- Deleting the general fallback -- its own plan
  ([cps-backend-n6-fallback-deletion-plan.md](cps-backend-n6-fallback-deletion-plan.md)).
- Changing abortive `shift` semantics in place (explicitly rejected above).
