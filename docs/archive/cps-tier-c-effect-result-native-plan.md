---
title: "CPS backend -- native emission + leak-free reap of Tier-C effect-result / resume-value crossings"
status: landed -- verified complete 2026-07-19 (see progress note below)
description: A colored function whose effect RESULT (or resume value / handler-case result) is a Tier-C by-value aggregate (a heap-boxed struct crossing the one-word DK slot) was SIG-REJECTed by fn_sig_ok and evicted to the direct/fiber effect runtime, where its boxes leak. Two gaps -- the signature gate never admitted the boxed-aggregate case it documents, and the DK-path Tier-C boxes (resume value, handler-case return, deliver) are read consume=false for multi-shot safety and only freed at the single-shot entry unwrap. This plan makes the shape native and makes ALL Tier-C boxes single-owned by the per-run reap list.
---

## Progress (2026-07-19)

**COMPLETE -- all five steps landed; verified against the tree.**

- Step 1 (return-gate widening): `fn_sig_ok` admits the return when
  `sig_slot_ok(rt) || slot_box_ty(rt)` -- `emit_cps_ir.c` carries the
  `slot_box_ty(rt)` widen at the return crossing (params stay on `slot_ty`, as
  specified). `slot_box_ty` / `sig_slot_ok` are in place.
- Steps 2-3 (register-at-creation): `slot_store_reap` is wired at every crossing
  the plan names -- `emit_deliver_ty` (KK_RET / KK_PROMPT), `emit_resume` (resume
  value), `emit_await` (future), and the perform-arg / `__eargs` sites.
- Step 4 (single-ownership reap): every load stays `consume=false`; the per-run
  `__dk_reap_run` is the sole owner. No `consume=true` double-free remains.
- Step 5: `cps-backend-tierc-effect` no longer carries any `requires.*` marker
  (only `input.tur` + `expected.stdout`), confirming it emits natively and passes
  the leak-checked suite. Sibling `cps-backend-tierc-return` / `-shift` / `-arg`
  fixtures are present.

Nothing tracked here remains open. The non-goals (owning-field aggregates,
markerless cloneable/serial/callcc leaks) were always out of scope and are tracked
elsewhere. Ready to archive.

## Symptom

`cps-backend-tierc-effect` (`(defeffect Get [] :Pr)`, a struct-returning effect
handled with a resuming case) leaks 2 heap boxes under ASan and its `run` /
`use-get` compile on the legacy **fiber** effect runtime
(`tur_effect_cont_resume`), not the DK path -- even though the sibling
`cps-backend-tierc-effect-arg` (struct *arg*, scalar result) is DK-native.

## Root cause -- two gaps

### Gap 1: `sig_slot_ok` never admits a boxed aggregate

`fn_sig_ok` gates a colored function's signature. Its comment says the return /
param check "widens from `slot_ty(kind)` to `slot_ty(kind) || slot_box_ty(type)`"
-- but `sig_slot_ok` (src/compiler/emit_cps_ir.c) actually returns just
`slot_ty(k)`. So any function whose return is a Tier-C by-value product
(`Pr`) SIG-REJECTs and evicts. `use-get` / `run` return `Pr`, so both evict; the
`Get` effect's `Pr` result then rides the fiber fallback. (`cps-backend-tierc-return`
is the same story -- its `outer` also SIG-REJECTs today, contradicting the
tier-c-crossing plan's "T-C1 landed native" note; T-C1 wired the box/unbox
machinery but the signature gate was never opened.)

**Fix (as landed):** widen the **return** gate only --
`fn_sig_ok` admits the return when `sig_slot_ok(rt) || slot_box_ty(rt)`. The box
machinery at the six DK boundaries and the by-value-product predicate are already
in place, so opening the return gate makes the shape emit natively.

Do **not** widen the *param* gate the same way: a by-value aggregate param crosses
in C by value, but the direct emitter's forward declaration passes an aggregate
param by pointer (`const T *`), so a CPS-emitted entry wrapper with a by-value
param signature conflicts with the forward decl (`conflicting types for 'run'`).
Widening `sig_slot_ok` globally regressed `conv-defstruct-typed-fn-field-lowering`
and `conv-adt-record-typed-fn-field-call` this way; keeping the param gate on
`slot_ty` only (aggregate params stay on the fallback) is correct until the CPS
param ABI matches the direct emitter's by-pointer convention.

### Gap 2: DK-path Tier-C boxes are never freed (multi-shot ownership)

Once native, the crossing boxes a Tier-C value with `slot_store`
(`(intptr_t)({ T *__bx = malloc; *__bx = v; __bx; })`) and unboxes it with
`slot_load`. A resume is multi-shot, so a box that rides a *replayed* value is
loaded `consume=false` (no free at the load) -- otherwise a second replay would
read freed memory. The only `consume=true` load is the single-shot **entry
unwrap** in the direct->cps wrapper. Result: the resume-value box (`emit_resume`)
and the handler-case-return / deliver boxes (`emit_deliver_ty`) leak.

Naively registering deliver boxes in the reap list double-frees: the box a
function delivers to the root becomes `__r` and IS freed by the entry unwrap
(`consume=true`). Confirmed against `cps-backend-tierc-return`.

## Design -- single-ownership by the per-run reap list

Make the per-run reap list (`__dk_reap_*`, from
docs/archive/cps-delimited-dk-node-leak.md) the **sole owner** of every Tier-C
box:

1. **Register at creation.** Every box-producing `slot_store` at a crossing
   becomes `slot_store_reap` (registers the box when `slot_box_ty` holds; a scalar
   is returned unchanged). Sites: `emit_deliver_ty` (KK_RET, KK_PROMPT),
   `emit_resume` (resume value), `emit_await` (future). The perform arg / `__eargs`
   sites already use `slot_store_reap`.
2. **Never free at a load.** Every `slot_load` stays `consume=false` -- including
   the entry unwrap, which stops freeing the box.
3. **Free once at the boundary.** `__dk_reap_run` (already gated on
   `!tur_async_suspended` and depth==0) frees each registered box exactly once,
   after the top-level `dk_run` has settled. The entry wrapper must **read the
   return value into a local BEFORE the reap** (the reap frees `__r`'s box), then
   return the local.

Soundness: each box is registered exactly once (at its creating `slot_store_reap`)
and freed exactly once (reap); no `consume=true` remains, so no double free. A
replay allocates fresh boxes, each registered and freed. On async-suspend the reap
is skipped (boxes leak rather than dangle, matching the `__root` handling).

## Steps

1. Widen the RETURN gate in `fn_sig_ok` with `slot_box_ty(rt)` (params stay on
   `slot_ty` -- see Gap 1). (Gap 1.)
2. `emit_deliver_ty`: `slot_store` -> `slot_store_reap` for KK_RET and KK_PROMPT.
3. `emit_resume`, `emit_await`: `slot_store` -> `slot_store_reap`.
4. Entry wrappers (d2b `main` + generic direct->cps): read the return value with
   `slot_load(consume=false)` into a local, emit `dk_free(__root)` + reap, then
   return the local. (Gap 2 / step 3.)
5. Regenerate snapshots; full suite; ASan sweep of the effect/struct-crossing
   fixtures for leaks AND double-frees; drop `cps-backend-tierc-effect`'s marker.

## Non-goals

- Owning-field aggregates (rc/ref/weak) crossing -- still gate item 4.
- The remaining pre-existing markerless leaks (cloneable/serial/callcc/escape/
  async machinery; the `dk_frame_resume` node) -- separate.
