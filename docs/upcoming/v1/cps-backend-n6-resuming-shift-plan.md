---
title: Resumable delimited control -- one substrate (k-reset / k-shift)
category: Planning
status: in progress (additive new surface; abortive shift + cloneable/serial converge into it)
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
continuation `k` (single-shot by default) and hands it to `receiver`, a
`(fn [k] ...)` that may invoke `k` zero or more times via the resume primitive.
This is the standard shift/reset, spelled with a `k-` prefix so it coexists with
the abortive `shift`/`reset` until those retire.

Semantics (matching the working cloneable model, minus the clone capability):

```turmeric
(defn step [k] : int
  (+ (k 1) (k 2)))          ; resume the delimited context twice? -> single-shot
                            ; variant errors on the 2nd resume; multi-shot uses k-shift*

(k-reset (+ 10 (k-shift step 0)))   ; context (+ 10 []): receiver gets k = (fn [x] (+ 10 x))
;; single-shot: (+ 10 1) = 11
```

- `receiver : (-> Cont<BodyT,ResetT> ResetT)` -- receives the reified
  continuation, NOT the body value (this is what distinguishes it from abortive
  shift). `body` is the shift's hole value / seed operand, mirroring the
  cloneable-shift 2-arg shape.
- The captured `k` is invoked with the resume primitive
  (`tur_cloneable_cont_resume` in the interpreter today; `dk_invoke` compiled).
- Single-shot by default; a multi-shot spelling can layer on later (it is just a
  cloneable continuation -- see "Consolidation").

## Work plan (incremental, each slice testable)

**Slice 1 -- LANDED.** `k-reset` / `k-shift` recognized as canonical resumable
delimited control, aliased onto the cloneable pipeline (the proven shared
substrate). Because the alias reuses the whole cloneable path, slices 2-4 below
came *for free* in this one move:

- Symbols `sym_k_reset` / `sym_k_shift` (`elab_internal.h`, `elab_core.c`);
  dispatch to `elab_cloneable_reset` / `elab_cloneable_shift` (`elab_call.c`).
- Type rule (2), direct emission on `dk_shift`/`dk_invoke` (3), and CT-IR
  lowering (4) are all inherited from the cloneable pipeline -- no new code.
- Fixture `k-shift-resumable-basic`: `k-shift` receiver resumes `k` (twice,
  cloneable); `direct == cps == turi == 23 / 6`.

Follow-on slices (the actual consolidation work, see below): a **single-shot**
`k-shift` specialization (lighter than the cloneable clone machinery), and
retiring abortive `shift` + `serial-*` into this surface.

## Consolidation (the retirement path)

Once `k-reset`/`k-shift` are solid:

- **Abortive `shift`** becomes sugar: `(shift f body)` == `(k-reset-less abort)`
  -- an abortive shift is a `k-shift` whose receiver ignores `k` and whose value
  aborts the prompt. Migrate the ~36 fixtures, then delete the abortive-specific
  interp/emit paths.
- **cloneable-shift / serial-shift** become `k-shift` with a *continuation
  capability* (cloneable = multi-shot clone; serial = marshalable). The capture
  machinery is already shared; only the capability annotation differs. Collapse
  the three receiver-lowering paths into one.

The end state is a single delimited-control primitive with a capability knob on
the continuation -- the "one substrate" this plan is named for.

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
