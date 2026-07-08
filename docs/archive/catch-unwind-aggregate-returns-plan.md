---
title: Stackless catch-unwind -- aggregate return types -- Plan
category: Planning
description: Widen the trampoline's single int64 value register (__v) so a trampolined function may RETURN a by-value aggregate (a C struct such as (Option int)), not just a scalar / int64-carrier. Follow-on to the completed general lowering (archived).
---

# Stackless catch-unwind -- aggregate return types -- Plan

> **Status: AR1 + AR2 DONE (single-function path). AR3 / AR4 deferred.** A
> trampolined self-recursive function may now RETURN a by-value aggregate (a C
> struct such as `(Option int)`). Option **(b)** from below was taken: the
> return value is **heap-boxed into `__v`** on produce and unboxed (memcpy +
> free) on every consume, mirroring the aggregate-PARAM solution (G6) -- `__v`
> stays `int64_t`, no `MAX_AGG` constant, no C-compile-error UX. All five `__v`
> consumers are aggregate-aware:
>
> 1. **`gs_deliver` `GSK_RETURN`** boxes an aggregate value (`malloc` +
>    `memcpy` from a temp) instead of `sc_save_expr`; the sink carries
>    `ret_ctype` + `ret_aggr`.
> 2. **self-call resume** (`gs_self_descend`) unboxes `__v` into a fresh
>    by-value local (`__ur<n>`) and frees the box.
> 3. **catch resume** keeps `tur_box_ok(__v)` unchanged -- an aggregate *thunk*
>    return is out of scope (AR3), gated out in `gs_collect` (bails to native)
>    so `__v` at a catch resume is always a scalar/int64 thunk value. The `tk`
>    fallback for a non-scalar (int64-carrier) thunk return is now `TY_INT`
>    (was the enclosing member's ret kind, which may now be an aggregate).
> 4. **DONE** unboxes `__v` into the C return struct and frees the box
>    (single-function `done_typed` path).
> 5. **propagate-zero** returns `(RetT){0}` on the escaping-panic path.
>
> A call-member suspension **temp** whose callee returns an aggregate is a
> by-value struct marked `is_aggr`, so it relocates across a further descend via
> the existing heap-box path and zero-inits as `= {0}`. Return classification
> reuses `gs_param_class` (the plan's suggestion); `gs_basic_ok` accepts a
> scalar / int64-carrier / by-value-aggregate return.
>
> **Group (AR4) stays deferred** for free: the G4 group path is gated by
> `sc_scalar_kind(result_kind)`, so an aggregate-returning function never enters
> the shared driver -- it takes the single-function path, or (if it needs mutual
> recursion) falls back to native.
>
> Covered by `stackless-catch-unwind-aggregate-return` (an `(Option int)`
> aggregate PARAM carried through recursion and RETURNED, with a non-tail
> recursive `(Option int)` feeding `some?` so the descend-unbox path runs,
> crossing a `catch-unwind`, 200,000-deep flat where a non-flat lowering would
> SIGSEGV). Full suite: 1976 passed, 0 failed. valgrind: the aggregate-return
> boxes are all freed on the happy path -- the run leaks byte-for-byte the same
> as the aggregate-PARAM baseline (only the pre-existing `tur_box_ok` result-box
> leak, matching native), i.e. AR adds zero new leak.

## Why this exists

The general catch-unwind lowering
([archived plan](../archive/compiled-catch-unwind-general-lowering-plan.md), G6)
made **by-value aggregate PARAMS** ride the trampoline (heap-boxed into a
`tur_cont` `saved[]` slot). It explicitly deferred aggregate **RETURN** types:
`gs_basic_ok` still requires the return type to pass `gs_slot_type` (scalar or
int64 carrier). A function whose return is a by-value struct (e.g.
`(defn f [...] : (Option int) ...)` returning `struct { bool; int64 }`) falls
back to native.

The blocker is the driver's **single value register**:

```c
int64_t __v = 0;
```

`__v` is the one 8-byte slot every inter-segment value flows through
(`src/compiler/emit_fns.c`): a producer stashes bits (`__v = <bits>; __pc =
__k->tag;`), and every consumer reads them back -- the self-call resume restore
(`sc_restore_expr(ret_ctype, ret_kind, "__v")`), the catch resume
(`tur_box_ok(__v)`), and DONE (`return sc_restore_expr(..., "__v")` single /
`return __v` group). Scalars, floats (bit-reinterpreted), and int64 carriers fit
in 8 bytes; a 16-byte `(Option int)` does not.

Unlike a param -- which has a private per-node `saved[]` home and got a private
heap box -- the return value *is* the shared `__v`, threaded through several
consumers plus the cross-function shim ABI. Supporting aggregate returns means
changing the register's representation and every producer/consumer of it.

## Options for the widened register

Decide by measurement; (b) is the smaller diff and mirrors the param solution.

- **(a) Widen `__v` to a max-size buffer / union.**
  `union { int64_t i; double d; unsigned char agg[MAX_AGG]; } __v;` (or a fixed
  `unsigned char __v[MAX_AGG]`), with `memcpy` in/out at every produce/consume
  site. `MAX_AGG` cannot be computed at emit time (`type_size_bytes` returns 0
  for composites), so it must be a generous constant guarded by
  `_Static_assert(sizeof(RetT) <= MAX_AGG)` -- which turns an oversized return
  into a C-compile error rather than a graceful fallback (bad UX).
- **(b) Heap-box the return value** (like aggregate params). A producer of an
  aggregate return does `void *__rb = malloc(sizeof(RetT)); memcpy(__rb, &val,
  sizeof(RetT)); __v = (int64_t)(intptr_t)__rb;`; every consumer memcpys out and
  frees. `__v` stays `int64_t`, no size known at emit time, no build-error risk.
  Cost: one malloc/free per aggregate-return delivery.

Recommendation: **(b)** for correctness and gracefulness first; consider (a) or a
hybrid (inline for small, box for large) only if malloc traffic on an
aggregate-returning hot path measures badly.

## Consumers of `__v` that must become aggregate-aware

Every site below is in `src/compiler/emit_fns.c` (grep `__v`):

1. **`gs_deliver` `GSK_RETURN`** -- the producer. Emit a boxed store for an
   aggregate ret kind instead of `sc_save_expr`.
2. **self-call resume restore** (`gs_self_descend`) -- reconstruct the callee's
   returned aggregate from `__v` into the caller's temp/hole var (unbox).
3. **catch resume** -- `tur_box_ok(__v)`: an aggregate *thunk* value boxed into
   an ok result. The ok box carries `int64_t`, so an aggregate ok-value has the
   same representation question as the return; keep aggregate thunks out of
   scope initially (require the thunk's return be an int64-slot type) unless the
   ok box itself is widened.
4. **DONE** -- single-function `return <aggregate from __v>` (unbox into the C
   return type); **group** `return __v` is raw int64 bits handed to the shim,
   which then `sc_restore_expr`s -- both need aggregate handling (see the group
   plan).
5. **G7 propagate-zero** -- `return sc_restore_expr(retctype, retkind,
   "INT64_C(0)")` on an escaping panic: an aggregate zero (`(RetT){0}`), value
   ignored by the caller but must type-check.

## Phases

- **AR1 -- classify the return. DONE.** `GsCtx.mem_ret_aggr[]` flags an
  aggregate-returning member; `GsSink` carries `ret_ctype` + `ret_aggr`.
  Classification reuses `gs_param_class` on the return type. The int64/scalar
  path is byte-identical when the return is not an aggregate (the whole suite is
  green with no snapshot churn).
- **AR2 -- box on produce, unbox on consume (single-function path). DONE.** All
  five consumers wired (see the status block up top). Differential-checked:
  `go(acc, 200000)` matches at small depth and runs flat where a non-flat
  lowering SIGSEGVs. valgrind: aggregate-return boxes all freed on the happy
  path -- byte-identical leak profile to the aggregate-param baseline (only the
  known `tur_box_ok` result-box leak). The bounded panic-path box leak noted in
  the params plan is unchanged (aggregate-param boxes on a popped self-call
  resume node are freed via `aggr_mask`; the result box itself matches native).
- **AR3 -- aggregate thunk values (optional). DEFERRED.** A catch whose thunk
  returns an aggregate needs the ok box to carry it (box-in-box or a widened
  result repr). Out of scope: `gs_collect` gates such a thunk out (bails to
  native) rather than emitting an ill-typed cast. Land when a fixture needs it.
- **AR4 -- group path. DEFERRED.** The shared driver returns `int64_t` and the
  shim restores; an aggregate return needs the driver to hand back a box pointer
  and the shim to unbox. Not yet wired -- the `sc_scalar_kind(result_kind)` gate
  on the group path keeps aggregate returns on the single-function path (or
  native for mutual recursion), so nothing mis-lowers in the meantime.

## Validation

- Default (flag-off) byte-identical; scalar/int64 returns byte-identical.
- New fixture: a trampolined function returning `(Option int)` (constructed only
  via allowed ops, or carried through), recursion crossing `catch-unwind`,
  matching native at small depth and running >=200,000 deep flat where native
  SIGSEGVs.
- valgrind: aggregate-return boxes freed on the happy path (same leak profile as
  the scalar version modulo the known result-box leak).

## Out of scope

- Aggregate PARAMS (done -- archived plan G6) and by-ptr aggregate params (own
  plan). Aggregate thunk *ok-values* beyond AR3.
