---
title: "CPS backend -- native emission of capturing-closure shift receivers (resuming shift, N6.3/N6-Task1)"
status: BLOCKED -- premise was a misreading; the real gap is the __Shift effect taint, not the receiver
description: A `shift` whose receiver is a CAPTURING closure that resumes the captured continuation (e.g. `(shift (fn [k] (k base)) 0)`, capturing `base`) is SIG/BODY-evicted with `EX_CLOSURE (capturing closure)` and falls back to the direct emitter's fiber shift lowering, where it leaks. Non-capturing resuming receivers (`(fn [k] (k 1))`, incl. multi-shot `(+ (k 1) (k 2))`) are ALREADY native via the `perform __Shift` desugar. This plan closes the remaining capturing case by binding `subk` and lifting the receiver body with its captures, instead of synthesizing an indirect `(recv val)` call that a fat closure cannot service.
---

## BLOCKED (2026-07-14): executing #1 disproved the premise

Attempting the approach revealed two things that invalidate this plan:

1. **The premise "`inner`/`twice`/`thrice` are already native" was WRONG** -- a
   misreading of `--dump-cps` (which prints the *built* colored IR, not whether
   the function actually EMITS native). The `TUR_TRACE_EVICT` trace and the
   emitted C are definitive: **none** of `inner`/`step`/`twice`/`thrice`/`run`
   emit a `*__cps` -- all evict to the direct/fiber emitter. `step` evicts
   `BODY-UNSUPPORTED (EX_CLOSURE)`; the others evict `BODY-STRUCT-OR-TAINT`. So
   the whole `__Shift`-based cross-function shift is on the fallback, not just the
   capturing case.

2. **The #1 change (admit a capturing closure as a delegated value) REGRESSES.**
   Wiring `is_delegatable_capturing_closure` into `cps_bind`/`cps_tail` made
   `step` build its receiver via CT_LETRAW, but the delegated `(recv k)` resume
   then routes through the fiber path and **taints the shared `__Shift` effect**,
   so `step` flips from `BODY-UNSUPPORTED` to `BODY-STRUCT-OR-TAINT` -- net zero
   native functions, and a broader taint. Reverted; no code landed.

**Real gap (re-scoped):** cross-function base shift lowers onto the synthetic
`__Shift` effect whose handler applies the receiver `(recv k)`; that application
(and/or the receiver crossing as the effect argument) taints `__Shift`, so every
user evicts. Making a colored function that performs/handles `__Shift` emit native
is the actual problem -- a `__Shift`-effect-taint / native-`(recv k)`-application
question, NOT a receiver-capture question. The capturing-closure delegation idea
(the EX_CALLCC analog) is sound in isolation but insufficient here.

## Root-cause map (2026-07-14, instrumented -- definitive)

Instrumenting the taint fixpoint (`TUR_TRACE_TAINT`) and `term_core_ok`
(`TUR_TRACE_TCO`) gave ground truth. The taint is a **consequence, not the
cause**: every `__Shift` user independently fails `term_core_ok` (`in_s=0`,
seed `core_ok=0`), so each is a genuine fiber function and they mutually taint
`__Shift`. Even a minimal NON-capturing pair (`inner`/`outer`, no `step`) all
evict. The `term_core_ok` failures, per node:

- **Performer** (`inner` = `(shift (fn [k] (k 1)) 0)` -> `perform __Shift(recv)`):
  `CT_PERFORM` fails `atom_ok` on the receiver argument -- the receiver is a
  `TY_FN` value and `atom_ok` admits neither `TY_FN` in `slot_ty` nor via
  `carrier_handle_ok` (heap-ADT only). (`body_ok=1 reset_ok=1 args_ok=0 caps=1`.)
  Admitting a bare (non-fat) `TY_FN` atom is necessary but NOT sufficient (below).
- **Handler** (`outer` = `(reset (+ 10 (inner)))` ->
  `(handle (+ 10 (inner)) (__Shift [recv] k) (recv k))`): `CT_HANDLE` fails BOTH
  `term_core_ok(handle.delim)` -- the handled body `(+ 10 (inner))` whose lowering
  calls the `__Shift` performer -- AND `reset_body_ok(handle.body)` (the handle's
  continuation). (`xslot=1 delim=0 body_ok=0`.) The handler CASE `(recv k)` is not
  even reached, but the indirect application of the receiver `TY_FN` to the
  continuation `k` is a further predicate to satisfy (`handle_case_ok`).

### The predicates are COUPLED -- no isolated slice (2026-07-14, tested)

Attempting the smallest "necessary" slice -- admit a bare (non-fat) `TY_FN`
receiver atom in `atom_ok` -- proved it cannot land in isolation. It is inert for
the shift fixture (the handler predicates still fail, so nothing goes native) but
it is NOT non-regressing: it **miscompiles 3 effectful-callback fixtures**
(`cps-backend-effectful-callback`, `effect-row-ho`, `effect-poly-typeclass` --
stdout mismatch, wrong values, not a build/snapshot change so it slips past the
snapshot regen). Root: `fn_sig_ok` admits an *effectful* fn param on purpose,
relying on the taint discipline -- the callback's effect (performed by the fiber
callback body) taints the effect so its handler stays co-fiber. Relaxing
`atom_ok` to admit a bare fn value let such a function flip to native (DK) while
its callback still performs on the fiber path, splitting handler and performer
across the two machines -> wrong output.

So the `atom_ok` relaxation must at least be scoped to **effect-free** fn values
(the `__Shift` receiver is effect-free; an effectful callback is not), AND it only
matters once the handler predicates are also satisfied -- i.e. the predicates are
**coupled** and must land as ONE coordinated, oracle-gated change, not as
independently-verifiable slices. Reverted; suite restored to green.

So native cross-function `__Shift` is a **multi-part feature**, not one predicate:
`atom_ok` (bare-fn receiver), `term_core_ok(delim)` (a body calling a `__Shift`
performer), `reset_body_ok` (the handler continuation), and native `(recv k)`
application in the handler case. Each gap independently evicts, so there is no
incremental single-predicate win -- the shape stays on the fallback until all are
satisfied together. This is n6-fallback Task 1 in full; it warrants its own
scoped project with the direct==cps oracle (`shift-crossfn-resume-works`) and an
ASan pass, not an incremental poke. `is_delegatable_capturing_closure` (the
receiver-capture idea) is orthogonal and would only matter after the bare-fn
receiver + handler gaps are closed.

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
