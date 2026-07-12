---
title: CPS backend N6 -- resuming (non-abortive) SHIFT
category: Planning
status: open (blocked on a language-level prerequisite -- see "Blocker")
description: Split out of cps-backend-n6-fallback-removal-followups-plan.md (Task 1). The current shift lowering is abortive on every path; a shift whose receiver invokes the captured continuation is not expressible today. Making it work is a language change (type rule + interpreter + direct emitter + CT-IR), not a CT-IR extension.
---

# CPS backend N6 -- resuming (non-abortive) SHIFT

## Where this came from

This was Task 1 of
[cps-backend-n6-fallback-removal-followups-plan.md](../../archive/cps-backend-n6-fallback-removal-followups-plan.md)
(now archived). Executing that plan established that the task as originally
framed -- "add a CT-IR lowering that binds `subk` and threads it into the
receiver" -- cannot be done as a self-contained CT-IR change, because a
resuming shift is not expressible or type-checkable in the language today. See
[docs/reported/cps-backend-n6-fallback-followups-blocked.md](../../reported/cps-backend-n6-fallback-followups-blocked.md)
for the full measurement/analysis.

## Blocker -- `shift` is abortive on every path

`(shift receiver body)` currently means "evaluate `body` to `v`, apply
`receiver` to `v`, deliver the result to the nearest reset." The receiver
receives the **body value**, never the continuation:

- Interpreter: `eval_abortive_shift` (`src/turi/eval.c:1561`) --
  `turi_call(env, fn, &v, 1)` then abort to `PROMPT_PLAIN`.
- Direct emitter: `emit_effects_shift` (`src/compiler/emit_effects.c:1429`) --
  emits `k_fn(body_val)`; comment: "Without CPS, we can't capture the
  continuation ... Full implementation requires CPS transformation."
- CT-IR backend: `cps_shift_body_kf` (`src/passes/cps_ir.c`) -- synthesizes the
  same `receiver(body)` application; `dk_shift` passes `subk` to the LH_SHIFT_BODY
  helper, which ignores it (`(void)subk;`, `emit_cps_ir.c` `emit_lifted`).

The **type rule** enforces the abortive shape: the receiver's parameter type
must equal the *body's* type. So the "resuming" form does not type-check:

```turmeric
(defn gen [] : int
  (reset (shift (fn [k : (-> int int)] (k 10)) 5)))
;; error [TUR-E0001]: shift: body type mismatch --
;; the continuation receiver expects (fn [int] : int), but the body has type int
```

Because no source program can express it, no `direct == cps` fixture is
constructible, and the CT-IR-only lowering would be dead code diverging from the
(abortive) direct/interp reference.

Prior art already reaches this conclusion:
[docs/archive/compiled-first-class-continuations-plan.md](../../archive/compiled-first-class-continuations-plan.md)
(lines 83-104): "Turmeric's `shift` lowering is **abortive** ... There is no
resumable-capture codegen path yet ... it is a plan of its own -- not a
mechanical extension of the existing abortive DK path."

## What "resuming shift" actually requires (scope)

A non-abortive `shift` is a **language feature**, landed as a coordinated change
across four surfaces so the reference (interp + direct) and the CPS backend
agree:

1. **Type rule.** Re-type the receiver as `(-> BodyT ResetT) -> ResetT` -- the
   receiver gets the *continuation* `(-> BodyT ResetT)`, not the body value.
   This is the breaking change; `shift-result-typing` and every existing
   `(shift (fn [v] ...) body)` fixture must be re-read under the new rule (the
   abortive identity `(fn [v] v)` becomes `(fn [k] (k body))` or similar).
2. **Interpreter.** Replace `eval_abortive_shift` with a resumable capture:
   reify the delimited context between the shift and its `reset` (the
   turi already does exactly this for `serial`/`cloneable` shift --
   `TuriCont`/`TsFrame`, `src/turi/eval.c:1601+`; reuse that machinery for plain
   `shift`), hand the receiver a `(-> BodyT ResetT)` that replays it.
3. **Direct emitter.** `emit_effects_shift` must capture the delimited
   continuation and pass it to the receiver instead of emitting `k_fn(body_val)`.
   The DK substrate already supports this (`dk_shift` captures `subk`,
   `dk_invoke` resumes it); the work is building the receiver-gets-`subk` shape
   the abortive emitter skips.
4. **CT-IR backend.** Only now does the original Task-1 idea apply: a
   `cps_shift_body` variant that binds `subk` and threads it into the receiver
   (receiver can `dk_invoke` it), selected when the receiver references the
   continuation; keep the abortive form as the fast path when it does not.

## Fixture (once 1-4 land)

A `shift` whose receiver calls its continuation (a generator/step shape),
`direct == cps`, LeakSanitizer-clean. Not constructible before step 1.

## Recommendation

This is a v-next language feature, not a v1 gate item. It should either be
sequenced behind an explicit decision to make `shift` non-abortive, or the
`shift` surface should be left abortive and resumable control kept on the
`perform`/`resume`/`handle` path (already landed: CT_PERFORM / CT_RESUME =
`dk_invoke`) and `cloneable`/`serial` shift. Gate item 7 does **not** depend on
this task (see the fallback-deletion plan): the general fallback's residuals are
ordinary forms, not resuming shifts.

## Out of scope

- The general fallback deletion -- its own plan
  ([cps-backend-n6-fallback-deletion-plan.md](cps-backend-n6-fallback-deletion-plan.md)).
- `perform`/`resume` (already the resumable path).
