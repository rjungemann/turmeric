---
title: Path B -- by-value HKT monomorphization across the van Laarhoven lens
  boundary (retire the carrier round-trip for wide functors)
category: Planning
description: Follow-up to van-laarhoven-wide-functor-carrier-plan (Path A / WF1-WF4).
  Path A boxes a wide by-value functor into the mode-B int64 carrier at each lens
  crossing so `view`/`set`/`over` and composition WORK with a `:copy`-struct
  functor -- but every crossing pays one heap box + copy + free. Path B retires
  those crossings by monomorphizing the lens body per concrete instantiating
  functor: `(f a)` is spelled by value end to end, no carrier, no box. MEDIUM-HIGH.
  Sliced. Deferred until Path A ships and a profile shows the box/unbox on a hot
  path; this plan is the pre-committed shape so the deferral is a decision with a
  trigger, not an indefinite punt.
---

# Path B -- by-value HKT monomorphization across the van Laarhoven lens boundary

## Context

Path A ([`van-laarhoven-wide-functor-carrier-plan.md`](van-laarhoven-wide-functor-carrier-plan.md),
slices WF1-WF4, landed 2026-07-03 behind `--enable=vl-wide-functor`) makes a
wide by-value functor work at the lens boundary by reusing MB2.5's
`emit_agg_box`/`emit_agg_unbox` bridge across the three lens-specific crossings
(the dict-dispatched method, the fat-boxed functor-wrapping `g`, and the lens's
own `(f S)` result). It is the same "auto-box at the abstract-`f` boundary"
shape the report
([../../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md))
listed as direction #3, threaded through the three crossings MB2.5 left open.

**Path A makes wide functors correct. It does not make them free.** Every wide
`(f a)` crossing pays:

- one heap allocation (`emit_agg_box` copies the aggregate onto the heap and
  hands the poly boundary a pointer disguised as int64),
- one `memcpy` back into a stack slot at the unbox site,
- one `free` at the drop site (`needs_drop_glue` on the boxed aggregate).

A lens-heavy hot loop (`view` inside a fold, `over` composed several deep) can
easily cross the boundary once per stage per iteration. For a two-word `:copy`
`Identity`, that is a heap alloc + copy + free per iteration per stage --
strictly worse than the carrier-compatible opaque path (which passes the value
in a single register).

**Path B is the zero-overhead answer** the report listed as direction #1 and
the parent plan named as the graduation: complete the by-value HKT
monomorphization direction (the `g_m7_hkt_enabled` machinery,
[../../guides/monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md))
across the lens boundary so a lens body invoked with `f := Identity` is emitted
as a specialization in which `(f a)` is spelled `Identity a` by value in every
signature, dispatch, and return -- no carrier, no box.

This plan is deliberately *not* scheduled today. It is committed as **the
graduation of `--enable=vl-wide-functor`**: when a profile shows Path A's
box/unbox on a hot path (the trigger named in WF's open question #4), we land
Path B, retire the box/unbox on the wide path, and graduate the flag to
default-on.

## Relationship to the wider monomorphization direction

Path B is not a new codegen strategy. It is the completion of the by-value HKT
work that started with `g_m7_hkt_enabled` (already the default since
2026-06-19; see [../tur-m7-hkt-flag-retirement-plan.md](../tur-m7-hkt-flag-retirement-plan.md))
across the *one* class of forall-body the carrier still services in production:
the van Laarhoven lens invoked with a wide by-value functor.

Concretely, the pieces already in place:

- **Signatures.** Functor / Applicative / Monad / Bifunctor / Foldable /
  Traversable class signatures are already in by-value form on disk (M7
  Layer-4). Their instances compile identically under both the carrier and
  by-value paths.
- **Dispatch.** MB2.5 (`emit_call_is_dict_param_dispatch`,
  `src/compiler/emit_core.c:1676`) already threads the aggregate box/unbox
  around the direct rank-2 shape `forall f. Functor f => (f int) -> (f int)`
  when the caller instantiates `f` with a wide `:copy` struct -- and does it
  without a specialization, on the shared carrier body.
- **Return-type threading.** `make_dict_clone` (`elab_call.c:5503`) tracks the
  clone's natural result type; WF3 opens the `!type_mentions_tyvar` gate for
  wide-aggregate results.

What is *not* in place for the lens shape:

- **Spec keying by functor.** The carrier body is one function for every `f`.
  Path B needs one emitted body per distinct `(f, a, b, s)` instantiation
  reached by any lens invocation, keyed off the resolved `Functor f` dictionary
  and the surrounding lens' focus/whole types.
- **By-value `(f a)` in every crossing.** The three WF crossings (dispatch
  operand, fat-`g` result, lens result) must be re-emitted with the aggregate
  spelled by value on both sides, not boxed and unboxed around a carrier slot.
- **Dispatch redirect.** At each lens call site, dispatch resolves to the
  specialized body directly, not to the carrier clone that boxes then dispatches.
- **Retirement of the box/unbox.** Once dispatch is routed, the Path A crossings
  are dead on the wide path; they stay live only for the shrinking
  carrier-compatible-functor path until the whole carrier is retired (out of
  scope for this plan; see "Non-goals").

## Slices

Each slice ships behind a fresh experiment flag (CLAUDE.md experimental-features
rule -- one fully-populated `EXPERIMENTS[]` row, a `g_opt_<name>` bool the
elaboration reads, an `experiment_warn_if_used` call, `expires_at` honored by
the release-cut skills) and adds fixtures before landing. Flags are independent
so we can stop after any slice; the graduation of `--enable=vl-wide-functor`
happens at VBM4 when VBM1-VBM3 make the box/unbox on the wide path unreachable
on the default (`vl-wide-mono` on) path.

### Slice VBM1 -- spec discovery: enumerate `(f, a, b, s)` for every lens site (`--enable=vl-wide-mono`)

**Goal.** For each call site whose resolved constraint pins `f` to a wide
by-value functor and reaches through the lens shape (nested-fn pin of
`g : (-> A (f A))`), record a **specialization key**
`(fn_def, functor_tyname, focus_ty, whole_ty)` so codegen can emit at most one
body per key.

**Work.**

- `src/compiler/elab_call.c` `elab_poly_call` -- when a poly call resolves and
  the pinned `f` is `emit_type_is_byvalue_adt` (already the WF3 gating
  predicate), instead of walking the WF3 result-box path unconditionally,
  register a spec key on the callee's `FnDef` (new `FnDef.mono_specs` vector,
  mirroring the existing `dict_clone_*` slots at `expr.h:945`).
- The key is content-hashed by fully-resolved type identity so distinct focus
  or whole types produce distinct specs, but structurally-equal instantiations
  reached from different call sites share one emit. Reuse the type-hash
  machinery `emit_module.c` uses for `__spec` naming.
- **No emit yet.** VBM1 only populates the registry; codegen still takes the
  Path A carrier-box path when the flag is on. This slice's fixtures are
  compile-time-only: `tur emit-c --dump-mono-specs` (a new debug dump behind
  the flag, mirroring `--dump-*` conventions) prints the spec key for the
  minimal repro from the report so we can review the keying by eye before
  wiring emit.

**Risk.** Low-medium. Purely additive; the shared `mono_specs` vector must be
allocated in the elaborator arena and not aliased across parent/child FnDefs
during method-level tyvar rewrite. Contained by keeping every read behind
`g_opt_vl_wide_mono`.

### Slice VBM2 -- emit a specialized lens body per `(f, a, b, s)` (folded into `--enable=vl-wide-mono`)

**Goal.** For each spec key registered in VBM1, emit one lens body in which
`(f a)`, `(f b)`, `(f S)`, and the functor-wrapping `g : (-> A (f A))` are
spelled by value with the concrete `f` substituted. The dispatch to `fmap`
inside the body becomes a **direct call to the specialized instance method**
for `f` (not an indirect through the dict), because the instance is resolved
at spec-emit time.

**Work.**

- `src/compiler/emit_fns.c` -- for each spec key on `FnDef.mono_specs`, emit
  a body whose C signature substitutes the concrete `f` into every use of the
  functor-tyvar. Name the emitted symbol `<basename>__mono_<hash>` mirroring
  MB2.5's `__spec` naming.
- `src/compiler/emit_expr.c` -- the three WF crossings become no-ops on the
  wide path in a spec body:
  - **WF1 (dispatch operand box/unbox).** The dispatch is no longer an
    indirect through the carrier dict; it is a direct call to the resolved
    `fmap` instance for the pinned `f`. The by-value `(f a)` argument and
    `(f b)` return flow through the natural ABI. No box, no unbox.
  - **WF2 (fat-`g` result unbox).** `g` is emitted with a by-value
    `(f A)` return; the invocation's C-level `struct` return needs no
    unbox.
  - **WF3 (lens result gate + box).** `result_full_type` mentions the
    tyvar, but the tyvar is substituted at spec-emit time to the concrete
    `f`, so the seam MB2 originally closed stays closed -- the spec body's
    result is a concrete by-value aggregate, not a tyvar-mentioning type.
- `src/compiler/emit_module.c` -- forward-declare each `__mono_<hash>` at the
  top of the translation unit alongside the existing `__spec` forwards.
- **Preserve WF3's error gating.** With `--enable=vl-wide-mono` off, TUR-E0309
  still fires exactly as today (Path A carrier-box path remains gated on
  `--enable=vl-wide-functor`). Only when both flags are on does the Path B
  emit path take over on the wide branch.

**Risk.** Medium. The genuinely new work is substituting the functor tyvar
through a full FnDef re-emit -- the elaborator already does this for M7 Layer-4
specs on class methods, but the lens body reaches through the surrounding
`elab_poly_call` scaffolding rather than the class-instance code path. The
mitigation is to lift the existing spec-body emit helper out of the class-instance
site and share it, rather than re-invent per-body substitution.

### Slice VBM3 -- dispatch redirect: point lens call sites at the spec (folded into `--enable=vl-wide-mono`)

**Goal.** At every lens invocation whose resolved `f` is a wide by-value
functor, the emitted C call targets the `__mono_<hash>` spec directly -- no
carrier dict clone, no fat-boxed `g`, no `emit_agg_box`/`emit_agg_unbox` around
the call.

**Work.**

- `src/compiler/emit_expr.c` -- in the poly-call emit
  (`:2745-2757`/`:2835-2845` -- the direct-shape box/unbox WF1/WF3 mirrored):
  before falling into the carrier-boxed path, look up the resolved
  `(f, a, b, s)` in the callee's `FnDef.mono_specs`. On hit, emit a direct call
  to the `__mono_<hash>` symbol with by-value arguments and a by-value return;
  skip the entire box/unbox scaffold. On miss (carrier-compatible functor, or
  flag off), fall through to the existing path unchanged.
- `src/compiler/elab_call.c` `make_dict_clone` -- when the resolved `f` matches
  a spec key on the callee, skip clone creation entirely on that call site.
  The clone is dead code for the specialized invocation; keep it live only for
  the carrier path.
- Fixture: byte-for-byte compare a Path B `emit-c` dump against a hand-written
  ideal to confirm zero `emit_agg_box`/`emit_agg_unbox` and zero `dict_clone`
  in the wide-functor lens fixtures.

**Risk.** Medium-high. This is the seam where Path A and Path B coexist on the
same call site (carrier-compatible functor: Path A untouched; wide functor:
Path B). Getting the branch wrong silently reintroduces the box/copy that the
plan exists to remove, or worse, mixes carrier and by-value ABIs on one call.
Mitigation: gate every new branch on
`emit_type_is_byvalue_adt(pinned_f) && g_opt_vl_wide_mono` and add a debug
assertion that either the direct spec call or the carrier clone is emitted,
never both.

### Slice VBM4 -- graduate `--enable=vl-wide-functor`; retire Path A on the wide path (folded into `--enable=vl-wide-mono` for one release, then default-on)

**Goal.** Once VBM1-VBM3 are on, the Path A box/unbox on the wide-functor
branch is unreachable. Delete it, graduate `--enable=vl-wide-functor` to
default-on (which retires TUR-E0309 for good), and eventually collapse
`--enable=vl-wide-mono` itself.

**Work.**

- Delete the WF1/WF2/WF3 box/unbox edits on the branch guarded by
  `emit_type_is_byvalue_adt(pinned_f)` (they remain on the carrier-compatible
  branch, which is untouched by this plan). Verify by removing the branch and
  confirming the wide-functor fixtures still pass on Path B.
- Delete TUR-E0309's `!g_opt_vl_wide_functor` gate; the diagnostic itself
  disappears from `elab_poly_call` (`elab_call.c:~5985`). Convert
  `tests/fixtures/errors/van-laarhoven-wide-byvalue-functor/` into (or delete
  in favor of) a passing acceptance fixture.
- Remove the `EXPERIMENTS[]` row for `vl-wide-functor` per the graduation
  procedure in [../../guides/experimental-flags-guide.md](../../guides/experimental-flags-guide.md).
- Fixtures matching the Path A shipped set, but with `--dump-mono-specs`
  asserting the spec is emitted and `emit-c` output free of aggregate-box
  calls: `van-laarhoven-lens-mono-{concrete,generic,compose,mixed}/`.
- Update [../../guides/lens-guide.md](../../guides/lens-guide.md): drop the
  Path-A note "a `:copy`-struct functor pays a heap box/unbox per crossing";
  state plainly that any Functor instance works with zero-overhead by-value
  dispatch under the default settings.
- Move [../../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md)
  to `docs/archive/` (per CLAUDE.md STRICT archiving rule) once
  `vl-wide-functor` is graduated.
- Deferred: retire `--enable=vl-wide-mono` itself in a follow-up release once
  the profile signal that motivated Path B is measurably closed and no
  regressions have surfaced. This mirrors the standard experiment-lifecycle
  cadence.

**Risk.** Low. Fixture + docs + deletion; the machinery is VBM1-VBM3.

## Non-goals

- **Retiring the mode-B carrier for carrier-compatible functors.** Opaque
  (`is_opaque`) and `:heap` functors already pass through the carrier as a
  single int64 word with zero copy; they are already zero-overhead. Path B
  targets the *wide-by-value* branch only. The carrier retirement for
  everything else is the M7 flag-retirement track ([../tur-m7-hkt-flag-retirement-plan.md](../tur-m7-hkt-flag-retirement-plan.md))
  and its follow-ups, not this plan.
- **Non-lens optics (Prism/Traversal), impredicative use, storing a
  constrained `forall` in a container.** All still out, matching the parent
  plans' gates.
- **Widening the mode-B carrier (Path C from the report).** Rejected in the
  parent plan; still rejected here.
- **Fixing the deferred `pure`/`empty` inference and method-level HKT tyvar
  items** ([../../reported/return-directed-methods-pure-empty-inference.md](../../reported/return-directed-methods-pure-empty-inference.md),
  [../../reported/class-method-level-hkt-tyvar-grounding.md](../../reported/class-method-level-hkt-tyvar-grounding.md)).
  Independent tracks; do not block Path B, and are not solved by it.

## Cost estimate (rough)

| Slice | Surface | Type system | Codegen | Tests/docs | Risk |
| --- | --- | --- | --- | --- | --- |
| VBM1 -- spec discovery/keying | small | small (FnDef.mono_specs) | none | small (emit-c dump fixture) | low-medium |
| VBM2 -- per-spec body emit | medium | medium (tyvar substitution) | medium | small | medium |
| VBM3 -- dispatch redirect | small | small (clone skip) | medium (branch) | medium (byte-compare) | medium-high |
| VBM4 -- graduate WF, retire Path A on wide | small | none | small (deletions) | small | low |

Path B is genuinely larger than Path A: it introduces a per-spec emit path
where Path A reused a bridge. But it is bounded -- the class of poly-body it
specializes is exactly "van Laarhoven lens whose pinned `f` is a wide by-value
aggregate," and every other poly body (Applicative, Monad, Traversable, the
direct MB2.5 shape, carrier-compatible functors) stays on its current path.

## Trigger (when Path B stops being deferred)

Land Path B when **any** of the following holds:

1. A profile of a shipped Turmeric program shows `emit_agg_box` /
   `emit_agg_unbox` from a lens-heavy loop in the top 5 hot functions.
2. A user (or spice author) reports the Path A allocation cost as a blocker
   for adopting van Laarhoven optics with a wide by-value functor over the
   record encoding.
3. The `--enable=vl-wide-functor` experiment approaches its `expires_at` date
   and the release-cut skills flag it as due for graduation or shelving.

Absent all three, Path B stays deferred and Path A is the shipped answer.

## Open questions

1. **Spec-body sharing across call sites.** Two lens call sites with the same
   `(fn_def, functor_tyname, focus_ty, whole_ty)` should share one emitted
   `__mono_<hash>` symbol. Confirm the content-hash keying is stable across
   invocation contexts (nothing accidentally distinguishes sites by lexical
   position or by which surrounding fn owns the call).
2. **Method-level tyvars.** A lens whose focus type is itself an unresolved
   method-level tyvar (the class of program in
   [../../reported/class-method-level-hkt-tyvar-grounding.md](../../reported/class-method-level-hkt-tyvar-grounding.md))
   cannot be specialized until that tyvar grounds. Path B should skip
   specialization on such sites and defer them to the Path A carrier bridge --
   or the report's residual should be fixed first, so every reachable site has
   a groundable focus type at spec-emit time. Decide the sequencing before
   VBM2.
3. **Drop glue on the by-value result.** A spec body returning
   `Identity a` by value where `Identity` has `needs_drop_glue` on a payload
   field is fine (the caller's stack slot owns the drop) -- but a spec body
   that internally builds and discards intermediate `(f a)` values must not
   double-drop. Confirm the existing by-value ADT drop discipline covers the
   spec-body case before VBM2 fixtures land.
4. **Composition across mixed functors.** A lens composed from one
   carrier-compatible-functor stage and one wide-by-value-functor stage
   (VBM4's `mixed` fixture) must dispatch stage-by-stage: the
   carrier-compatible stage stays on the shared carrier body; the wide stage
   dispatches to its spec. Confirm the dispatch-redirect keys on the pinned
   `f` at each stage, not at the outermost invocation.
5. **Interaction with the carrier retirement track.** If the M7 carrier
   retirement lands before Path B ships, the "carrier-compatible functor"
   branch also becomes a monomorphized emit -- at which point Path B
   generalizes to "specialize every lens body per instantiating functor" and
   the wide/narrow branch collapses. That is a strictly better outcome and
   worth watching, but not a reason to hold Path B.

## Related

- [`van-laarhoven-wide-functor-carrier-plan.md`](van-laarhoven-wide-functor-carrier-plan.md)
  -- Path A (WF1-WF4), which Path B graduates and whose box/unbox on the wide
  branch it retires.
- [../../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md)
  -- the tracked gap; direction #1 is the shape this plan realizes.
- [`constrained-hkt-forall-mode-b-plan.md`](constrained-hkt-forall-mode-b-plan.md)
  -- MB1-MB4 + MB2.5 (the direct-shape aggregate bridge Path B replaces on the
  lens shape).
- [../tur-m7-hkt-flag-retirement-plan.md](../tur-m7-hkt-flag-retirement-plan.md)
  -- the wider carrier retirement track this plan aligns with without depending
  on.
- [../../guides/monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md)
  -- the `g_m7_hkt_enabled` by-value-HKT machinery Path B completes across the
  lens boundary.
- [../../guides/lens-guide.md](../../guides/lens-guide.md) -- the shipped lens
  guide (VBM4 removes the Path A cost note).
- [../../guides/experimental-flags-guide.md](../../guides/experimental-flags-guide.md)
  -- the experiment-lifecycle procedure Path B ships and graduates under.
- `src/compiler/emit_expr.c:440-460` -- `emit_agg_box`/`emit_agg_unbox`, the
  Path A bridge Path B retires on the wide branch.
- `src/compiler/emit_expr.c:2745-2757`, `:2835-2845` -- the direct-shape
  poly-carrier box/unbox VBM3 branches around.
- `src/compiler/emit_core.c:1676` -- `emit_call_is_dict_param_dispatch`, the
  WF1 site VBM2 replaces with a direct spec call on the wide branch.
- `src/compiler/elab_call.c:5503` -- `make_dict_clone`, the WF3 seam VBM3
  skips on spec-key hit.
- `src/compiler/emit_fns.c` -- the class-instance `__spec` emit VBM2 lifts and
  shares with the lens spec emit.
