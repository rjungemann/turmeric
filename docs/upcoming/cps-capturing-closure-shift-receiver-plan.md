---
title: "CPS backend -- native emission of capturing-closure shift receivers (resuming shift, N6.3/N6-Task1)"
status: REFRAMED -- the real gap is general "native handle-in-reset" (a KK_PROMPT-delivering nested handle), NOT __Shift/receiver-specific; needs its own core delimited-control plan
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

## FEASIBILITY RESOLVED (2026-07-14): feasible via the cloneable-cont bridge

Attempting the coordinated change settled the open question -- it is **feasible**,
not architecturally blocked, and the mechanism is the EXISTING DK<->cloneable
bridge. Evidence chain:

- The receiver (`__fn_1282`, the `(fn [k] (k 1))`) is COLORED but emitted only as
  a direct entry `static int64_t __fn_1282(int64_t k)` whose body resumes via
  `tur_cloneable_cont_resume(tur_continuation_snapshot(k), 1)` -- the cloneable-
  cont machinery, NOT `dk_invoke`. `(recv k)` in the handler calls it dynamically
  by fn-pointer value (only the direct entry is reachable through a value).
- That looked like a hard machine-coherence blocker (a DK subk can't be resumed
  through `tur_cloneable_cont_resume`). It is NOT: `emit_cps_cloneable_bridge_prelude`
  already emits `__dk_cont_fn` -- a `tur_cloneable_cont` whose env is a DK chain
  and whose resume dispatches to `dk_invoke`. So a DK subk WRAPPED via that bridge
  (`tur_cloneable_cont_alloc(__dk_cont_fn, dk_copy_range(subk,NULL), __dk_env_clone,
  __dk_env_drop)`) resumes correctly through the receiver's existing
  `tur_cloneable_cont_resume`. The two continuation machines interoperate at this
  exact seam.

### What the coordinated change requires (complete map)

To make cross-function `__Shift` native, ALL of the following must land together
(they are coupled -- no isolated slice, proven above):

1. **`atom_ok`** admits an **effect-free** bare (non-fat) `TY_FN` atom (the
   receiver). Must exclude effectful fn values or it miscompiles the effectful-
   callback taint discipline (proven).
2. **`fn_sig_ok`** for the performer + handler + receiver (the receiver's
   `cont -> int` param / `CT_RESUME` body must be admitted).
3. **`term_core_ok(handle.delim)`** -- a body that calls a `__Shift` performer.
4. **`reset_body_ok`** for the handler continuation.
5. **`handle_case_ok`** for the `(recv k)` case (a dynamic call of the receiver
   value with the continuation).
6. **New emit (the crux):** the DK `__Shift` handler case must BRIDGE-WRAP `subk`
   via `__dk_cont_fn` before the dynamic `(recv k)` call, so the receiver's
   `tur_cloneable_cont_resume` resumes the DK chain. This is the only genuinely
   new codegen; the rest are admission-predicate widenings.

### Implementation attempt (2026-07-14): the relaxations must be `__Shift`-SCOPED

Pushing into the implementation established a hard constraint the design missed.
Step 1 (relax `atom_ok` to admit a bare fn atom) cannot be gated by general
slot-representability -- not even scoped to "effect-free":

- `effect_row` on the CROSSING atom's type is unreliable. A lambda-lifted
  effectful callback (`(fn [] (perform (Ask)))`, type `(fn [] #fx{Ask} int)`)
  reaches `atom_ok` with `a->type->as.fn.effect_row == NULL` (the row is erased
  through lifting), so an "effect-free `TY_FN`" check WRONGLY admits it.
- Consequence (third miscompile, tested): admitting it flips `run`/`apply-cb`
  (`cps-backend-effectful-callback`) to native -- `Ask`'s handler becomes a DK
  `dk_handler` while the callback still performs `Ask` on the fiber path -->
  `tur: unhandled effect (tag 2)` -> abort. General slot-widening breaks the
  effectful-callback taint discipline regardless of an effect-row check.

So EVERY relaxation must be scoped to the `__Shift` effect specifically -- admit
the receiver arg ONLY at a `CT_PERFORM`/`CT_HANDLE` whose effect is `__Shift`,
never for atoms/effects in general. That threads an "is this `__Shift`?" test
through `term_core_ok` (CT_PERFORM + CT_HANDLE), `reset_body_ok`,
`handle_case_ok`, AND the emit -- and some failures are entangled: the handler's
`term_core_ok(delim)` fails partly because `delim` calls the (not-yet-admissible)
performer, a circular dependency with the performer's own admission. This is a
larger, more coupled change than "admission widenings"; the general-`atom_ok`
shortcut is a dead end.

### ROOT REFRAME (2026-07-14): the gap is "handle nested in a reset", not `__Shift`

The implementation attempt's exact CT-IR trace + a reduction test reframe the gap
entirely. It is NOT `__Shift`- or receiver-specific:

- CT-IR of the `__Shift` desugar's handler is `reset { handle {..} with __Shift..
  in (<prompt> v) }` -- a **handle nested inside a reset**, whose continuation
  delivers to the enclosing reset's prompt (`KK_PROMPT`).
- `term_core_ok`'s `CT_APPCONT` case returns false for `KK_PROMPT` delivery, so
  `reset_body_ok(handle.body)` rejects that continuation -> the handle evicts.
- **Reduction test (decisive):** a PLAIN handle-in-reset with no shift at all --
  `(reset (+ 100 (handle (use-e) (E [] k) (resume k 5))))` -- ALSO evicts
  (`BODY-STRUCT-OR-TAINT`, 0 `__cps`), while the same handle NOT inside a reset is
  native (`cps-backend-effect`). So the missing capability is **native emission of
  a handle nested in a reset**; cross-function `__Shift` is merely one instance
  (its desugar produces exactly that nesting).

Consequences for this plan:
- The fix is NOT `__Shift`-scopable -- it is a general delimited-control admission
  + emit change for a `KK_PROMPT`-delivering handle continuation (and the
  heap-join `delim`, and the receiver atom for the `__Shift` case specifically).
  It touches core `term_core_ok` / reset-handle admission with broad blast radius.
- The right next artifact is a **"native handle-in-reset"** plan (general), with
  cross-function `__Shift` as a downstream beneficiary, gated by a plain
  handle-in-reset oracle FIRST (simpler than the shift oracle), then the shift +
  effectful-callback oracles.

### Status / recommendation

Feasible and fully mapped, but a substantial multi-site implementation whose crux
(step 6) is new codegen. It must land as one change gated by BOTH oracles:
`shift-crossfn-resume-works` (direct==cps = 11/105/23/306, single- AND multi-shot)
AND the effectful-callback set (`cps-backend-effectful-callback`, `effect-row-ho`,
`effect-poly-typeclass`) which the `atom_ok` widening can break. Not landed here:
writing step 6's emit speculatively risked a third miscompile, so this hands off a
verified design rather than broken code. Multi-shot (`twice`/`thrice`) needs the
bridge's `dk_copy`/clone discipline to give each resume an independent snapshot --
already what `__dk_env_clone` provides, but verify under ASan.

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
