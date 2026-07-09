---
title: CPS backend -- classify colored-generic monomorphs
status: scoping
description: The last large N6 fallback lever. A colored *generic* (polymorphic) function is CPS-classified only as its generic template, which sig-rejects on a TY_APP-with-tyvar param/return; the concrete monomorphs the direct emitter actually generates (choose_or__spec__...) are never CPS-emitted, so any DK effect chain that touches one collapses to the fiber machine. This doc scopes the architecture, the soundness situation, the real value, and a phased implementation path.
---

# CPS backend -- classify colored-generic monomorphs

## The gap

A colored **generic** function -- one with a type parameter whose signature
carries a `TY_APP`-with-tyvar param or return (`(defn f [A] [x : (Option A)] :
A ...)`) -- is CPS-classified only as its **generic template**. That template
sig-rejects (`fn_sig_ok`: a `TY_APP` with a free type variable is neither a
scalar, a by-value box, nor a heap carrier), so the function falls back.

The concrete **monomorphs** the direct emitter actually generates and calls
(`choose_or__spec__int64_t_tur_adt_Option__int_int64_t(tur_adt_Option__int o,
int64_t dflt)`) -- whose params *are* concrete by-value ADTs that would pass
`fn_sig_ok` -- are never seen by the CPS classifier. They run on the **fiber**
effect machine (`tur_fiber_block_new` / `EffectHandlerFrame`), never DK.

### Why it is the largest raw lever, and why the raw count overstates it

`sig-param TY_APP` measured ~4352 (plus `sig-ret TY_APP` ~625), the biggest
single fallback bucket. But:

- A **concrete** `TY_APP` (`(Option int)`, `(Pair int int)`, `(Result int
  int)`) already CPS-emits -- it monomorphizes to a by-value owning-free ADT and
  rides the Tier C box path. Verified `direct == cps`.
- The remaining surface is **generic templates** -- the count is inflated by
  per-fixture stdlib re-emission, and much of it is generics whose monomorphs
  never enter a DK effect chain at all (a self-contained combinator that both
  performs and handles its own effect is already correct on fiber).

So the *real* value is narrower than 4352: it is **letting a DK effect chain
include generic monomorphs** -- a colored generic that performs an effect handled
by a CPS peer (or vice versa) -- instead of the whole chain collapsing to fiber.

## Current behavior is SOUND (do not treat this as a bug)

The whole-program effect co-classification in `ensure_S` (the taint fixpoint)
keeps this correct today: a generic monomorph performs on the fiber machine, and
any CPS peer that shares that effect is **evicted** from S (Rule B taint), so a
performer and its handler are never split across the DK and fiber machines. This
was verified:

```turmeric
(defeffect A [] :int)
(defn emit-twice [T] [x : T] : int { (perform (A)) + (perform (A)) })  ; generic -> fiber monomorph
(defn run [] : int (handle (emit-twice 0) (A [] k) (resume k 5)))       ; concrete handler
```

`run` is evicted from CPS (effect A tainted by the fiber monomorph) and the whole
chain runs on fiber -- `direct == cps == 10`. The gap is **coverage, not
correctness**: these functions never get the DK treatment, but they never
miscompile.

## Why this is not a `fn_sig_ok` widening (the architectural crux)

The two passes run at different times over different representations:

| Pass | When | Sees |
| --- | --- | --- |
| CPS classification (`ensure_S`) | a whole-program **pre-emit** walk of `program->as.program.items` | top-level `EX_FN_DEF` **generic templates** + concrete fns |
| Monomorphization | **lazily during emit**, as the direct emitter walks call sites (`emit_abi_intern_spec` interns an `EmitAbiSpecialization` per `(generic, arg_types, result_type)`) | concrete monomorphs, born as `EmitAbiSpecialization`, never as `FnDef` program items |

So when `ensure_S` runs, the monomorphs **do not exist yet**. There is nothing
concrete to classify. Widening `fn_sig_ok` to accept a tyvar `TY_APP` param would
only admit the *generic template* (carrier-ABI, int64) -- but nothing calls the
generic template; every call site is rewritten to a concrete monomorph
(`emit_abi_register_call` records the call->spec mapping). A CPS'd generic
template would be dead code.

## The one existing hook point

`emit_fn_def` (emit_fns.c) is re-invoked for **each** monomorph body with
`ctx->current_abi_specialization` set to that spec (`use_abi_spec =
ctx->current_abi_specialization->fn == fd`). It calls `emit_cps_ir_try_fn`
(emit_fns.c:2613) at the top -- so the CPS backend **is** entered once per
monomorph. Today `emit_cps_ir_try_fn` ignores the active specialization: it
looks the generic `fd` up in the `ensure_S` cache (which sig-rejected) and
returns false, falling through to the direct spec emit.

That is the seam to widen: when a specialization is active, translate the generic
body **under the spec's type substitution** into a monomorph CTerm, classify
*that*, and -- if it lands in the subset -- emit `<spec->clone_name>__cps` + the
entry wrapper, routing the call site to it.

## The hard part: whole-program co-classification over monomorphs

`emit_cps_ir_try_fn` per-monomorph gets the *body* emitted, but S-membership is
not a per-function decision -- it is a **whole-program fixpoint** (Rule A: drop
heap-join-needing candidates; Rule B: evict any S function sharing a tainted
effect). That fixpoint currently enumerates only the program's top-level
`FnDef`s. To admit monomorphs it must enumerate them too, which means their
effect sets must be known **before** the per-monomorph emit -- but monomorphs are
discovered lazily *during* emit.

Two ways to break the ordering knot:

1. **Pre-emit monomorph discovery.** Add a pre-pass (before `ensure_S`'s
   fixpoint) that walks call sites and interns the same `(generic, arg_types,
   result_type)` keys the direct emitter's `emit_abi_intern_spec` would, so
   `ensure_S` can enumerate every monomorph, translate each under substitution,
   collect its effect set, and run the taint fixpoint over the full monomorph
   set. Emit-time then reuses the decisions. This duplicates the direct
   emitter's spec-discovery logic (or refactors it to run once, up front, shared
   by both).
2. **Two-phase, conservative.** Keep classification keyed on generics but treat a
   colored generic as *tentatively CPS-able*; at monomorph-emit time, if all
   effects the monomorph touches are still in S (not tainted by any fiber peer),
   emit the DK version, else fall back per-monomorph. Risk: a monomorph emitted
   early could commit to DK before a later fiber peer taints the shared effect,
   breaking the invariant -- so this needs the effect taint to be resolved
   up front regardless, which collapses back into (1)'s discovery pass.

(1) is the honest design. The cost is a shared monomorph-discovery pass; the
payoff is that both the direct emitter and the CPS classifier work from one
enumerated spec set.

## Phased plan

- **G1 -- translate a generic body under substitution + report admissibility.
  LANDED (analysis-only).** Rather than deep-copy the body with substituted
  types, the probe translates the generic body as-is (`cps_ir_translate_fn`) and
  makes the *gate* substitution-aware: a `g_cps_mono_resolver` hook (an `EmitCtx
  *`, non-NULL only during the probe) is consulted by `slot_ok_t`, which resolves
  each type through the active `EmitAbiSpecialization` (`emit_resolve_type`)
  before gating. So the existing `fn_sig_ok` / `term_core_ok` run **unchanged**
  but see the monomorph's concrete types. Wired behind `--dump-cps-mono`
  (`g_dump_cps_mono`), fired from `emit_cps_ir_try_fn` when
  `ctx->current_abi_specialization` pins a colored generic; prints
  `[cps-mono] <clone_name>: sig=.. body=.. => ADMISSIBLE|fallback`. The
  `slot_ok_t` hook is a strict no-op when the flag is off (full suite: 2037
  passed, 0 failed -- zero emit change). Verified: `choose-or` (`(Option A)`
  param), `wrapit` (`(Option A)` return), and `firstish` (`(Pair A B)` param) all
  report **ADMISSIBLE** at their `int` monomorphs -- the substitution-translation
  works for the stdlib-parametric-ADT surface (the bulk of `sig-param TY_APP`).

  *Finding for G2 (resolver completeness).* A **user-defstruct** generic
  (`(defstruct Box [A] (val A) (tag :int))`, `boxup : (Box A)`) reports a **false
  `fallback`**: its *concrete* analog (a `defstruct BoxI [val tag]` monomorphic
  fn) CPS-emits fine, but `emit_resolve_type` resolves `(Box A)` to a `(Box int)`
  `TY_APP` **without attaching the monomorphized `AdtDef`**, so `slot_box_ty`
  cannot see it is an owning-free by-value product. Stdlib ADTs dodge this
  because their monomorphs are pre-registered. G2/G3 must materialize the full
  monomorph type (the direct emitter's `emit_abi_instantiate_type` + monomorph
  ADT registration), not just `emit_resolve_type`, to admit user-defstruct
  generics. The probe is honest about this: it *under*-reports (never claims a
  fallback monomorph is admissible), so it is a safe lower bound on the surface.

  Only a scalar-tyvar generic (`f [T] [x : T]`, `T`=int) has no distinct
  `__spec__` clone -- it emits once as the int64-carrier base generic, so the
  probe (which keys on an active specialization) does not cover it; that is a
  separate carrier-generic sub-case, not part of the `TY_APP` monomorph surface.

- **G2 -- shared monomorph-discovery pass.** Factor the direct emitter's spec
  interning so the set of `(generic, arg_types, result_type)` monomorphs is
  enumerable before the per-item emit. Feed it into `ensure_S` so the taint
  fixpoint ranges over monomorphs as well as top-level fns. Still no emit change:
  assert (under the dump flag) that the S-set including monomorphs is consistent
  with today's fiber behavior (no monomorph that shares an effect with a fiber-
  only peer is marked S).

- **G3 -- emit the monomorph DK body + route the call site.** In
  `emit_cps_ir_try_fn`, when `current_abi_specialization` is set and the
  monomorph is in S, emit `<clone_name>__cps` + the entry wrapper (mirroring the
  top-level path but with the spec's concrete types), and make
  `emit_abi_register_call`'s call->spec routing point DK-context callers at the
  `__cps` clone. Gate the whole thing on `--enable=cps-backend`. Fixtures: the
  cross-machine `emit-twice`/`run` case now CPS-emits end to end
  (`direct == cps`), plus a self-contained generic monomorph.

- **G4 -- widen `fn_sig_ok` for the concrete monomorph types.** With monomorphs
  classified under their concrete param/return types, the tyvar-`TY_APP`
  rejection no longer applies to them (they are concrete by-value ADTs / heap
  carriers, already admitted). The generic *template* still sig-rejects and stays
  dead -- that is correct.

## Status / recommendation

**G1 is landed** (analysis-only, zero emit change) -- the substitution-translation
is proven for the stdlib-parametric-ADT surface, and G1 surfaced the concrete G2
requirement: the type resolver must materialize the full monomorph ADT (not just
`emit_resolve_type`) to cover user-defstruct generics.

G2-G4 remain a materially larger lift than the prior N6 slices (which were
single-pass gate widenings). They touch classification ordering, the
specialization pipeline, and emit routing -- G2 in particular refactors shared
discovery logic and upgrades the resolver. This is the right next big lever *if*
we want to close the generic surface, but it is not a one-sitting surgical
change, and the current behavior is already sound (effect-taint keeps generic
monomorphs on fiber without splitting any DK chain).

Next-step options, in the spirit of the one-track-to-v1 priority:

- **Proceed to G2** -- factor a shared monomorph-discovery pass that enumerates
  specs before `ensure_S`'s taint fixpoint, and upgrade the probe's resolver to
  the full monomorph-ADT materialization (closing the user-defstruct
  false-negative). Still landable dump-only before any emit change.
- **Defer G2-G4** and keep picking off smaller residuals (the remaining small
  items are effectful callbacks and the delimited-control tail), returning to
  this once those are exhausted.

## Depends on / reuses

- `EmitAbiSpecialization` + `emit_abi_intern_spec` (emit_module.c) -- the spec
  registry to enumerate/reuse.
- `ensure_S` taint fixpoint (emit_cps_ir.c) -- must range over monomorphs.
- `emit_cps_ir_try_fn` (emit_cps_ir.c) -- the per-fn (now per-monomorph) hook.
- Parent: [cps-backend-n6-fallback-removal-plan.md](cps-backend-n6-fallback-removal-plan.md).
