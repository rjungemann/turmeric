---
title: CPS backend -- owning values captured into a continuation env (the env-capture story; O3)
category: Planning
status: open (not started) -- gives O3 from the owning-pointers follow-ups a real home
description: The N6 CT-IR backend lifts a continuation body into a leaked, shallow-copied env struct. `cap_ty_ok` cuts any owning capture (rc/carrier-ADT/owning-aggregate) to the whole-function fallback, admitting one only for a provably single-shot continuation (`g_cap_single_shot`), where the body's drop runs at most once. This plan is the "env-capture story" that O3 in cps-backend-owning-pointers-followups-plan.md keeps forward-referencing: make the CT-IR backend clone/incref an owning capture on continuation clone and decref it on drop, so a multi-shot continuation that captures an owning value CPS-emits correctly instead of falling back. Until this lands the cut is sound (fallback handles it), so this is missed coverage, not a correctness gap.
---

# CPS backend -- owning values captured into a continuation env (O3)

## Why this document exists

O3 in
[cps-backend-owning-pointers-followups-plan.md](cps-backend-owning-pointers-followups-plan.md)
("captured owning values") is deferred with the words *"rides the env-capture
story ... owned by whichever plan lifts the zero-capture cut."* That story had
**no document** -- the same phrase recurs in
[compiled-first-class-continuations-followups-plan.md](compiled-first-class-continuations-followups-plan.md)
(`Phase F2`, "keep it behind the same guard until the env-capture / deep-clone
story lands") and in the archived parent
[cps-backend-owning-pointers-plan.md](../../archive/cps-backend-owning-pointers-plan.md),
each pointing forward, none of them *being* the plan. This document is that plan.

**Nothing here is a correctness gap today.** The cut described below is sound:
every case it rejects is handled correctly by the general whole-function
fallback. This is a *coverage* item -- extend the CT-IR backend so these cases
CPS-emit instead of falling back -- and it is one of the things N6.5 (the general
fallback deletion,
[cps-backend-n6-fallback-removal-followups-plan.md](cps-backend-n6-fallback-removal-followups-plan.md))
needs covered before it can delete the fallback without hard-erroring an
owning-capture continuation.

## The mechanism today (verified)

A colored function whose control op (reset / handle / perform / shift) has a
continuation body is lowered by lifting that body into a top-level helper plus an
**env struct** carrying the body's free locals (its "captures"):

- `emit_cont_env` (`src/compiler/emit_cps_ir.c:2882`) `malloc`s a `<hname>_env`,
  writes `->__k` (the enclosing continuation) and `->f0..fN` from each captured
  binding by a **shallow value copy** (`->fI = <cvar>;`, `:2893-2896`). The
  comment on the function is explicit: *"The struct is leaked with the DK
  nodes."* There is no clone on the way in and no drop on the way out.
- The lifted frame fn reads them straight back out
  (`<ctype> <cn> = __cap->fI;`, `:2740-2748`).
- When the DK chain itself is copied for a multi-shot resume, `dk_copy_node`
  (`src/runtime/cps_prompt.c:73`) copies the frame **spine** but shallow-copies
  the env pointer (`c->env = n->env;`, `:75`). Every resume shares one env, hence
  one copy of each owning capture.

Because the env is shared and leaked with no retain/drop, only **Copy** captures
are safe there. `cap_ty_ok` (`:609`) encodes exactly that: a capture may ride the
env iff it is a scalar (`slot_ty`) or an owning-free by-value aggregate
(`slot_box_ty`). An owning handle (`rc` / `weak` / `lref`), a carrier ADT, or an
aggregate with an owning field fails the gate and `cap_add` sets `cs->ok = false`
(`:635`) -> the whole function falls back.

The **one** admitted owning case is the single-shot exception (`owning_ok =
g_cap_single_shot`, `:634`): a reset / shift-body / perform continuation runs at
most once, so a drop the body performs on the shared capture runs at most once
and is balanced against the single shallow copy. `collect_caps` sets the flag
(`:848`); a handler **case** body runs once *per perform* (multi-shot), so
`collect_caps_case` forces it off (`:864`) and a handler case that captures an
owning value falls back. `^borrow` captures ride safely regardless (the callee
never drops them; `cap_add:628`).

So the open hole is precisely: **an owning value captured by a genuinely
multi-shot continuation** (a handler case, or a reset/shift that is resumed more
than once). N shallow copies of one +1 refcount, N body drops -> N-1 double
frees. The gate keeps it unreachable by falling back; this plan makes it emit.

## The reusable substrate (verified)

We do not need new runtime primitives -- the multi-shot machinery already exists
for the cloneable / delimited-control forms and is proven correct there:

- `tur_cloneable_cont_alloc(fn, cap, clone, drop)` -- a **refcounted**
  continuation with per-clone / per-drop callbacks
  (`emit_cps_ir.c:3373`; builtins `tur_cloneable_cont_{clone,drop,resume}`,
  `src/compiler/builtins.c:197-199`). The cloneable shift path already threads an
  owning capture through it.
- The owning-value **clone/drop glue** on the value side: `rc_strong_increment`
  (incref; `emit_expr.c:5668`) and `rc_strong_decrement` / the `drop!` builtin
  (decref; `emit_expr.c`, `emit_module.c`), plus the generated struct / ADT drop
  glue keyed by `type_uses_carrier_abi` (Tier A carrier) and
  `adt_is_byvalue_product` (Tier C) that O2 already emits for owning fields of an
  aggregate.

What is missing is only the wiring: the CT-IR leaked-env frame
(`emit_cont_env` + `dk_frame`) currently attaches **no** clone/drop callbacks,
and `__dk_env_clone` / `__dk_env_drop` (`emit_dk_runtime.c:57-60`) clone/free the
DK **spine** only (`dk_copy_range` / `dk_free`), never the owning payload inside
a frame env.

## Design

Two viable disciplines. Recommend **B**; **A** is a smaller stepping stone.

### Option A -- incref-on-read-out (leak-tolerant, minimal)

Keep the env leaked (matching the existing leaked-DK-node policy,
`docs/reported/cps-delimited-dk-node-leak.md`). When the lifted frame fn loads an
**owning** capture out of the env, emit an incref/clone instead of a bare read:
`<ctype> <cn> = <clone-glue>(__cap->fI);` (`emit_cps_ir.c:2748`). Each invocation
then owns its own +1, which the body's existing drop (`drop!` / `ref<T>`
auto-drop / linear-consume) balances. The env's original +1 is intentionally
leaked, exactly like the env struct and the DK nodes around it.

- **Pro:** tiny, local change; no teardown path; reuses `rc_strong_increment`
  and the O2 aggregate clone glue directly.
- **Con:** every owning capture *leaks* its heap payload (the env's +1 is never
  reclaimed). Multi-shot owning-capture fixtures must carry
  `requires.no-leak-check` (the compiler/codegen path stays leak-checked; only
  the spawned program opts out). That is acceptable under the current
  leaked-DK-node regime but is debt.

### Option B -- refcounted env with clone/drop (recommended)

Stop leaking the owning-carrying env; put it under the same refcount discipline
the cloneable path uses. Give the lifted continuation frame a real clone/drop
pair:

- **clone** (run by `dk_copy_node` when a multi-shot resume copies the spine):
  deep-copy the env struct and, for each owning field, run its incref/clone glue
  (`rc_strong_increment` for an rc handle; the O2 struct/ADT clone glue for an
  aggregate with owning fields). Copy fields stay a shallow copy.
- **drop** (run when a DK node / cont is freed): for each owning field, run its
  decref/drop glue, then `free` the env.

Concretely: teach `emit_cont_env` to emit a `<hname>_env_clone` /
`<hname>_env_drop` pair whenever `caps` contains an owning field, and route the
frame through the refcounted `tur_cloneable_cont` (or attach the callbacks to the
`dk_frame` and give the CT-IR reset/handle DK nodes a teardown, wiring `dk_free`
to invoke the env drop). Copy-only envs keep today's leaked-struct fast path
unchanged -- the clone/drop pair is emitted only when an owning field is present.

- **Pro:** leak-clean; multi-shot owning-capture fixtures run under normal leak
  detection; unifies the CT-IR env with the already-proven cloneable clone/drop
  discipline (fewer divergent capture stories, helps the marshal/reset
  unification in
  [cps-backend-unification-marshal-reset-unification-plan.md](../v2/cps-backend-unification-marshal-reset-unification-plan.md)).
- **Con:** the CT-IR reset/handle DK nodes are leaked today; giving *just the
  owning-env frames* a teardown means the DK node that owns such a frame must
  run its env drop at free time. That is the real work -- a scoped teardown for
  owning-carrying frames without disturbing the leaked fast path for Copy-only
  frames.

### Gate change (both options)

`cap_ty_ok` / `cap_add` stop being the hard cut. Introduce
`cap_owning_ok(ty, type)` -- true when the capture is an owning kind for which we
can emit clone + drop glue (rc handle, carrier ADT, or an owning-carrying
`slot_box`/aggregate whose O2 glue exists). `cap_add` admits it (recording that
field needs clone/drop) instead of setting `cs->ok = false`, for the multi-shot
path too. `g_cap_single_shot` stays as the fast path: a single-shot owning
capture still needs neither clone nor drop (one shallow copy, one body drop), so
keep emitting it as today and only engage the clone/drop machinery for the
multi-shot admissions. A capture whose owning kind has no emittable glue (should
be none once O2 covers the aggregate cases) still falls back -- never a silent
unsound admit.

## Phases

### E1 -- gate + single-owning-capture, Option A (unblock coverage)
Add `cap_owning_ok` and the incref-on-read-out emit. Admit a **single** owning
capture (rc handle) into a multi-shot handler-case continuation. Fixture
`tests/fixtures/cps-backend-owning-capture-handler-case`: a handler case body
that captures one `rc` local, is resumed twice, drops it once per resume;
`direct == cps`, carries `requires.no-leak-check` (Option A leaks the env +1).

### E2 -- aggregate + carrier-ADT owning captures (Option A)
Extend to a captured aggregate with owning fields and a captured carrier ADT,
reusing the O2 struct/ADT clone glue for the read-out incref. Fixture with a
captured `defstruct` holding an `rc` field.

### E3 -- refcounted env (Option B), leak-clean
Emit `<hname>_env_clone` / `<hname>_env_drop`, route owning-carrying frames
through the refcounted cont / DK teardown, and drop `requires.no-leak-check` from
the E1/E2 fixtures (they now run under normal leak detection). Copy-only envs
keep the leaked fast path. This is the graduation-quality landing.

### E4 -- reset/shift multi-shot owning capture
Cover a reset/shift resumed more than once whose body captures an owning value
(the non-handler multi-shot shape). Confirms the discipline is not
handler-case-specific. Fixture: a generator/step shift, resumed twice, capturing
an `rc`.

Land E1-E2 to unblock coverage; E3 is required before this counts as done for
N6.5's leak-clean bar. E4 rounds out the shapes.

## Interaction with other plans

- **O3** in the owning-pointers follow-ups is *this*. Update its "State" line to
  point here instead of to an unnamed story.
- **F2** in the first-class-continuations follow-ups (multi-shot effect resume
  that captures an owning value) is the same hazard surfaced through the effect
  surface; it should ride E1-E3 and drop its "until the env-capture story lands"
  guard when E3 lands.
- **N6.5** (fallback deletion) needs owning-capture continuations covered (E1-E4)
  or explicitly carved out; a hard error on an owning-capture continuation would
  regress coverage. List this plan as a prerequisite for N6.5's "no residual
  general fallback" bar, or add owning-capture to N6.5's named carve-out with
  justification if E-series has not landed.

## Depends on / reuses

- `cap_ty_ok` / `cap_add` / `collect_caps` / `g_cap_single_shot`
  (`src/compiler/emit_cps_ir.c`) -- the gate to relax.
- `emit_cont_env` (`emit_cps_ir.c:2882`) + the lifted frame read-out
  (`:2740-2748`) -- the emit sites to extend.
- `tur_cloneable_cont_alloc` / `_clone` / `_drop` / `_resume`
  (`emit_cps_ir.c:3373`, `builtins.c:197-199`) -- the refcounted multi-shot
  substrate for Option B.
- `dk_copy_node` / `dk_free` (`src/runtime/cps_prompt.c`) and
  `__dk_env_clone` / `__dk_env_drop` (`emit_dk_runtime.c:57-60`) -- the DK
  clone/free hooks Option B threads the env drop through.
- Owning clone/drop glue: `rc_strong_increment` (`emit_expr.c:5668`),
  `rc_strong_decrement` / `drop!` (`emit_expr.c`, `emit_module.c`), and the O2
  struct/ADT drop glue (`type_uses_carrier_abi`, `adt_is_byvalue_product`).

## Out of scope

- **`ref<T>` scope-exit auto-drop in a colored function** -- that is O1-b, a
  distinct hole (the `EX_DEFER` node has no CT-IR lowering case, so it falls back
  before capture even enters the picture). Tracked in the owning-pointers
  follow-ups; not this plan.
- **The cloneable / serial / async delimited-control forms** -- they already
  carry owning captures correctly via their own clone/drop path (`emit_cps.c`);
  N6.5 keeps them carved out. This plan is only about the N6 CT-IR backend's
  leaked-env frames.
- **Adding owning pointers to `slot_ty`** -- Findings 1-2 of the parent stand:
  owning pointers never cross the slot bare, so there is nothing to hook.
