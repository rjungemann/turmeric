# DK-native multishot continuations for user resumable-payload effects

**Status:** COMPLETE -- Phases A + B + C LANDED. The WHOLE B1 family CPS-emits
(`effect-cont-kv-sugar`, `multishot-effect-cont-kv-sugar`,
`effect-fn-payload-capturing`, `cross-function-resume-via-effect`). Prepared from
investigations P-inv .. P-inv3 in `cps-runtime-finish-plan.md`.

> **CONFIRMED DONE (2026-07-19).** All four target fixtures still present. The
> `resumable_payload` / `reflavor_effect_payload` / `multishot-effect-cont`
> machinery is live in `src/` (elab_effects.c, elab_fns.c, emit_cps_ir.c,
> effect.c/.h, cps_ir.c). The `cps-tramp-resume` experiment has since GRADUATED
> (always-on) and the fiber effect runtime is deleted (Stage G), so this whole
> family now DK-lowers by default. No phase remains open -- ready to archive.

## Phase C result (landed) -- raw-`int` continuation payloads

`effect-fn-payload-capturing` (`f : (fn [int] int)`, `(resume k v)`) and
`cross-function-resume-via-effect` (`seed : (fn [int] int)`) now CPS-emit (1107 /
11, ASan-clean, direct == cps == turi). The `int` carries no cont flavor for
`defeffect` to detect (and a genuine `(fn [int] R)` payload must not be
disturbed), so the continuation is identified by the reliable FORM signal that the
payload body RESUMES its first param -- `(resume k ...)` or the `(k ...)` sugar
(`form_lambda_resumes_first_param`). When that fires, the payload's `int` param is
reflavored to `multishot-effect-cont` (which type-checks: a TY_CONT payload
unifies with the declared `(fn [int] R)` int-carrier param) and the shared effect
object is marked `resumable_payload_param` at the perform site, so the enclosing
handler (elaborated after the performer) reuses ALL the Phase A wiring
(perform-arg gate, `is_effect_payload`, handler `k` upgrade + cloneable wrap).
Order note: the int case relies on the performer being elaborated before the
handler (top-level defns elaborate in source order); the annotated
`effect-cont`/`multishot-effect-cont` case stays order-independent (detected at
`defeffect`). A `(fn [int] R)` payload that never resumes its param is untouched
and stays on the fiber path.

## Phase B result (landed) -- and a corrected diagnosis

Phase B was scoped as "multi-shot resume." That was WRONG (verified): a
resumable-payload effect whose payload resumes twice (`(+ (k 1) (k 2))`) with a
DIRECT handled body already CPS-emits after Phase A (a minimal probe returns 3).
The real blocker for `multishot-effect-cont-kv-sugar` was its handled body
`(+ 10 (inner))` -- a **general** admission gap, not multishot/payload-specific: a
plain `(defeffect Ask [] :int)` with the same `(handle (+ 10 (inner)) ...)` shape
evicted identically. A top-level `handle`'s handled body was admitted with the
strict `term_core_ok`, which forbids the interior KK_PROMPT delivery that a
computation join emits (`(+ 10 (inner))` lowers to
`letcont j(t){+10 t; <prompt>} in tailcall inner(j)`), so it evicted to fiber.

Fix: a new `handle_delim_ok` (emit_cps_ir.c) admits the top-level handled body
like `term_core_ok` but permitting KK_PROMPT delivery through pure-computation /
join / cps->cps-tailcall shapes. It deliberately does NOT admit an interior
CONTROL op or a delegated (`CT_LETRAW`) call in the handled body: a
direct-emitted effectful call performs on the FIBER runtime, not the handle's DK
prompt, so `(handle (f) ...)` with an effectful fn-param `f` must stay on the
fiber path (relaxing it made E "unhandled"). Interior effects reach the DK prompt
only through a cps->cps tailcall. `multishot-effect-cont-kv-sugar` now CPS-emits
(23, ASan-clean), and the general shape `(handle (+ 10 (colored-call)) ...)` now
CPS-emits for plain effects too. Suite 2179/0.

Landing was iterative: the first cut (relax the delim to the existing `delim_ok`)
regressed 3 fixtures (`effect-error-codes`, `effect-handler-capture-nested`,
`handle-effectful-fn-param-same-fn`) by admitting interior control ops / delegated
effectful calls that the top-level `emit_handle` cannot lower; `handle_delim_ok`
is the precise predicate that admits the win without those.

## Phase A result (landed)

`effect-cont-kv-sugar` now CPS-emits on BOTH the performer (`producer`) and the
handler (`run`): `BODY-UNSUPPORTED`/`BODY-STRUCT-OR-TAINT` both cleared, output
1107 on direct == cps == turi, ASan-clean on the CPS path, full suite 2179/0.
Implemented exactly as designed, landed as one unit:

1. `reflavor_effect_payload` (elab_effects.c) rewrites a `(fn [k : effect-cont] ...)`
   payload's cont param to `multishot-effect-cont` at the perform site.
2. A per-effect `resumable_payload_param` (set at `defeffect` from the param type
   FORM -- the cont flavor collapses to its TY_INT carrier in the stored Type, so
   the form is the reliable signal) drives both sides.
3. The handler case auto-upgrades an un-annotated `k` to `CK_MULTISHOT` (fixing
   the cloneable-runtime emission gate) and carries a precise `resumable_payload`
   flag.
4. CPS admission: `PerformExpr.resumable_payload` widens the perform-arg gate to
   the boxed-fn payload atom; `Closure.is_effect_payload` admits a capturing
   payload in `is_delegatable_value`.
5. CPS emit: the handler-case cloneable-cont wrap + boxed-payload `arg` reap
   (`emit_cps_ir.c` ~3350) fires on `hcase->resumable_payload` (generalizing the
   `is_shift_effect` gate), scoped precisely so a hand-written `^multishot`
   handler on a NON-payload effect (whose `arg` is not a boxed pointer) is
   untouched.

**Note:** `multishot-effect-cont-kv-sugar` was NOT flipped by Phase A alone --
but not for the reason first assumed (the double-resume). Its blocker was the
`(+ 10 (inner))` handled body, a general handle-delim admission gap fixed in
Phase B (below). After Phase B it CPS-emits (its pre-existing 32 B fiber-path leak
in `tur_cloneable_cont_alloc` disappears with it).

**One-line:** teach the CPS/DK backend to CPS-emit a USER algebraic effect whose
handler resumes THROUGH a fn payload (`(defeffect E [f : (fn [cont] R)] ...)`,
handler `(E [f] k) (f k)`) -- the general case of the cross-function `shift`
(`__Shift`) machinery, which today is the ONLY resumable-payload shape that
CPS-emits. This is the single capability that unblocks the whole B1 family
(`effect-fn-payload-capturing`, `effect-cont-kv-sugar`,
`cross-function-resume-via-effect`) and their clean multishot siblings
(`multishot-effect-cont-kv-sugar` and the capture-free one-shot variants), all of
which currently evict to the fiber runtime.

## Why this is the right target

The B1 fixtures do not have three different problems -- they have ONE. All user
resumable-payload effects (one-shot OR multishot, capturing OR capture-free)
evict to the fiber runtime today; the taint fixpoint does this CORRECTLY given
the current admission rules (verified P-inv3: `multishot-effect-cont-kv-sugar`
and a minimal capture-free one-shot `effect-cont` payload both evict on
`BODY-STRUCT-OR-TAINT` and run via `tur_handler_dispatch`). Only the
fully-synthesized `__Shift` desugar CPS-emits. A capturing payload surfaces as
`BODY-UNSUPPORTED` (`EX_CLOSURE`) purely because the closure trips CT-IR
translation before the taint fixpoint evicts it; admitting the closure alone just
moves the eviction to `BODY-STRUCT-OR-TAINT` (reshuffling). So there is no
smaller Phase-1 slice here -- CPS-emitting these requires the DK-native
resumable-payload capability described below, landed as one coherent unit.

## Root cause (verified)

The taint chain, for the canonical `inner` performs / `outer` handles pair:

1. **Performer fails admission.** `term_core_ok(inner)` is FALSE: the
   `CT_PERFORM` arg gate (`emit_cps_ir.c` ~1642) admits a bare `TY_FN` payload
   atom ONLY via `shift_recv_atom_ok`, which is gated on
   `is_shift_effect(effect)`. A user effect's fn payload atom fails `atom_ok` and
   is not `__Shift`, so the perform is rejected and `inner` is not a candidate.
2. **Effect gets tainted.** `inner` is kept in the classification table with
   `in_s=false` (`ensure_S`, ~2460); its performed effect `ShiftE` is added to
   the taint set because a non-CPS (fiber) peer performs it.
3. **Handler is evicted by co-classification.** `outer`'s body DOES admit
   (`(f k)` lowers to a `CT_LETRAW`, `handle_case_ok` passes), so `outer` starts
   as a candidate -- but the taint fixpoint evicts it because it HANDLES a tainted
   effect: the DK and fiber effect machines do not interoperate, so a handler must
   be co-classified with its performers.

Net: the entire effect co-evicts to fiber, rooted at the performer's perform-arg
gate. This is why the naive `is_effect_payload` admission patch (P-inv3) is
UNSOUND: widening the perform-arg `atom_ok` flips `inner`'s `in_s` true and
un-taints `ShiftE`, so `outer` CPS-emits -- but the handler still binds `k` as a
raw `DK *` (the cloneable-wrap at ~3350 is `is_shift_effect`-scoped) and hands it
to a payload that resumes via the fiber substrate. Runtime miscompile:
`continuation error: not a capturable continuation`. Admission MUST land together
with the handler cloneable-wrap and the resume-substrate reflavor.

## What already exists and is reusable

The DK-native multishot continuation PRIMITIVE is already built and proven -- by
`__Shift` and by the native cloneable-reset (`CT_CLONEABLE`) path:

- **DK-backed cloneable cont.**
  `tur_cloneable_cont_alloc(__dk_cont_fn, dk_chain_env, __dk_env_clone,
  __dk_env_drop)` wraps a `dk_copy_range` chain as a generic `cont_fn(env,value)`
  continuation. Resuming it runs `__dk_cont_fn = dk_invoke`, snapshotting
  (`__dk_env_clone = dk_copy_range`) before each resume so multi-shot
  (`(+ (k 1) (k 2))`) resumes independent chains. Built at `emit_cps_ir.c` ~3373
  (`__Shift` handler case) and ~4012 (`CT_CLONEABLE`).
- **Multishot resume lowering.** `emit_effects_resume` (`emit_effects.c` ~1228)
  already routes a `CK_MULTISHOT` continuation through
  `tur_cloneable_cont_resume(tur_continuation_snapshot(k), v)` -- the SAME
  DK-backed path. A payload whose cont param is `CK_MULTISHOT` therefore resumes
  through the substrate the CPS handler produces.
- **Form-level cont reflavor.** `reflavor_shift_receiver` /
  `elab_specialize_cont_receiver` / `reflavor_cont_sym` (`elab_effects.c`
  ~410-540) rewrite a receiver lambda's `cont` param to
  `multishot-effect-cont` before elaboration, so the whole closure elaborates
  against the multishot flavor.
- **Payload boxing + the `is_effect_payload` flag idea.** `elab_perform`
  (~1415) already boxes an fn payload into the one-word `{thunk,env}` slot; the
  reverted P-inv patch showed where to mark a capturing payload delegatable.
- **Leak discipline.** The `__dk_reap_keep` / `__dk_reap_ptr` boundary reaps
  (P3.c/P3.d) already reclaim the `__Shift` cloneable cont + receiver env; the
  same reaps apply here.

The gap is purely that all of this is SCOPED TO `__Shift` (by effect name) and to
the synthesized shift/reset desugar. Generalizing the gates from
"`is_shift_effect`" to "this handler case's continuation is resumed through a
boxed-fn payload whose cont param is cloneable/multishot" is the work.

## Design

Land as ONE unit (partial landing miscompiles, per P-inv3):

1. **Elaboration -- reflavor the payload cont param.** In `elab_perform`, when an
   arg is a closure payload for an effect whose corresponding param is
   `(fn [cont-ish] R)`, reflavor its cont param to `multishot-effect-cont` at the
   FORM level before elaboration (reuse `reflavor_cont_sym` with
   `effect-cont`/`cont` -> `multishot-effect-cont`). This makes the payload's
   `resume`/`(k v)` lower to `tur_cloneable_cont_resume` on both the direct and
   CPS paths, keeping `direct == cps == turi`. Mark the (capturing) payload
   `is_effect_payload` so the CPS backend delegates its build.
   - Multishot is the sound generalization of one-shot (resume-once behaves
     identically: snapshot once, resume once), so reflavoring a one-shot
     `effect-cont` payload up to multishot does not change observable behaviour --
     confirmed by the existing `^multishot`/one-shot fixtures running identically
     on the fiber path.
2. **CPS admission -- perform-arg gate.** Generalize `shift_recv_atom_ok`'s use at
   `emit_cps_ir.c` ~1642 from `is_shift_effect && ...` to "the arg is a boxed-fn
   payload of a resumable-payload effect." A `TY_FN` atom reaches a perform arg
   ONLY as a boxed payload, so the relaxation stays scoped to the perform-arg gate
   (never the generic `atom_ok`, per the `cps-native-handle-in-reset-plan.md`
   "Dead ends" note).
3. **CPS admission -- handler case.** The handler `(f k)` already admits as a
   `CT_LETRAW`; verify `handle_case_ok` + `collect_caps_case` admit the
   multishot-cont `k` binding (it is bound, not slot-crossed). No change expected,
   but this is the spot to watch.
4. **Emit -- handler-case cloneable-wrap.** Generalize the `is_shift_effect`
   branch at `emit_cps_ir.c` ~3350 to fire for ANY handler case whose continuation
   `k` is consumed by a boxed-fn payload / is multishot-flavored: build `k` as a
   `tur_cloneable_cont` over `dk_copy_range(subk, NULL)` and reap it + the payload
   `arg` at the boundary (the existing P3.c/P3.d reaps). Key it on a
   `CHandleCase` flag (`bridge_payload_cont`) set in `build_handle` when the
   effect's payload param is a resumable cont-fn and the case resumes through it --
   NOT on the effect name.
5. **Taint fixpoint.** With (2)+(4) making performer and handler admissible, the
   fixpoint admits the whole effect automatically (no fiber peer). No fixpoint
   change is expected; verify no unrelated fiber peer of the same effect forces
   co-eviction.

## Soundness requirements (hard gates)

- **All-or-nothing.** Admission (2), reflavor (1), and handler-wrap (4) MUST land
  together. Any subset that admits the performer without the DK cloneable handler
  + cloneable resume reproduces the P-inv3 miscompile.
- **`direct == cps == turi` parity.** The reflavor changes the direct/interpreter
  resume lowering too (to cloneable). Every B1 fixture and its siblings must
  produce identical output on all three backends. The fiber path already supports
  multishot user effects (`tur_handler_dispatch` + `__tur_msdyn_cont`), so this
  should hold, but it is the top regression risk and must be checked per fixture.
- **Multishot linearity.** Multi-shot resume through a callback is a hard error on
  both paths today (TUR-E0201); the new path must preserve that diagnostic where
  it applies, and must not silently double-free a cont (the snapshot-before-resume
  discipline in `emit_effects_resume` + boundary reaps handle this -- verify under
  ASan).
- **Leak-clean.** The handler cloneable cont, its DK-chain env, and the payload
  box must be boundary-reaped (P3.c/P3.d reaps generalize). ASan target on
  `effect-cont-kv-sugar` + `multishot-effect-cont-kv-sugar` must be clean.

## Phasing

- **Phase A (one-shot `effect-cont`, capturing):** `effect-cont-kv-sugar`. Reflavor
  + admission + handler-wrap. First end-to-end proof.
- **Phase B (explicit multishot):** `multishot-effect-cont-kv-sugar` -- should fall
  out of Phase A (same machinery); it currently evicts to fiber and should flip to
  CPS. Validates true multi-shot (`(+ (k 1) (k 2))`) on DK.
- **Phase C (`int`-typed payload cont):** `effect-fn-payload-capturing`,
  `cross-function-resume-via-effect`. A raw-`int` cont handle has no `copy_kind` to
  reflavor. Options: (i) treat a `(fn [int] R)` payload of a resumable-payload
  effect AS a cont param and reflavor by position (the effect declares it a
  continuation-consuming payload); or (ii) require the source to annotate
  `effect-cont`. Decide during Phase A/B; (i) is preferred (no source change) but
  needs the effect-decl to mark which payload param is the continuation.

## Risks / open questions

- **Parity regressions** (top risk): the reflavor is a global lowering change.
  Mitigate by running the full suite + the three-backend diff per B1 fixture.
- **Which payload param is "the continuation"?** For `int`-typed payloads there is
  no cont annotation. The effect declaration may need to mark the
  continuation-consuming payload param (Phase C).
- **Nested / re-entrant handlers.** A resumable-payload effect handled inside
  another handler -- verify the DK chain copy ranges compose (the `__Shift` path
  already exercises nesting via reset, but user handlers nest differently).
- **Interaction with `handle-shallow`** (`h->shallow`): confirm the cloneable-wrap
  is correct for a shallow handler (single-shot re-install) vs deep.
- **Effort:** medium-large. Reuses proven primitives, but touches effect
  elaboration + perform/handler admission + handler emit + resume lowering, and
  carries real substrate/parity risk. Comparable in size to the original
  `__Shift` cross-function-resume feature, of which this is the generalization.

## Test targets & exit gate

- Fixtures flip from evicted to CPS-emitted and keep passing on all three
  backends: `effect-cont-kv-sugar`, `multishot-effect-cont-kv-sugar`,
  `effect-fn-payload-capturing`, `cross-function-resume-via-effect` (Phase C).
- `TUR_TRACE_EVICT`: the B1 fixtures drop out of `BODY-UNSUPPORTED` AND
  `BODY-STRUCT-OR-TAINT` (not merely move between them).
- ASan clean on the above; TUR-E0201 multi-shot-through-callback diagnostic
  preserved where it applies.
- Full `bash tests/run.sh` green (no parity regressions), snapshots regenerated.
