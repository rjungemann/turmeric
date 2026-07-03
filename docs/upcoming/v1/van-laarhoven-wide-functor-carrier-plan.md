---
title: Wide by-value functors through the van Laarhoven lens boundary -- lifting
  the one-int64-carrier restriction
category: Planning
description: Follow-up to constrained-hkt-forall-mode-b-plan (MB2.5/MB4). Let a
  van Laarhoven lens focus through a functor whose `(f a)` is a WIDE by-value
  aggregate (a `:copy` struct / single-variant flat-product ADT), not just a
  one-int64 carrier-compatible opaque. Today such a functor is rejected up front
  with TUR-E0309 (the diagnostic that replaced a silent segfault); this plan
  retires that restriction. MEDIUM. Sliced. Path A first.
---

# Wide by-value functors through the van Laarhoven lens boundary

## Context

The van Laarhoven optic

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

ships and runs on Turmeric today for `view`/`set`/`over`/composition
([`constrained-hkt-forall-mode-b-plan.md`](constrained-hkt-forall-mode-b-plan.md),
slices MB1-MB4; fixtures `van-laarhoven-lens-{concrete,generic,compose,delegate}/`).
But every one of those fixtures declares its functors as **one-int64 carriers**:

```turmeric
(defopaque Const    [r a] :int)
(defopaque Identity [a]   :int)
```

That is forced, not incidental. The mode-B dispatch threads the abstract functor
`(f a)` through the erased `tur_poly_fn_t` **int64 carrier** (one word). A functor
whose `(f a)` is a **wide by-value aggregate** -- a `:copy` struct or a
single-variant flat-product ADT wider than one word -- does not fit that slot.
Until 2026-07-03 the elaborator accepted such a program and codegen emitted
`int64<->struct-pointer` reinterpretations that segfaulted at runtime (or failed
the C stage under a strict `-Wint-conversion` compiler). That silent-miscompile
half is now closed: the mode-B re-discharge loop (`src/compiler/elab_call.c`,
`elab_poly_call`) rejects a wide-by-value functor pinned through the lens's
nested `g : (-> A (f A))` parameter with **TUR-E0309**
([../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md),
fixture `errors/van-laarhoven-wide-byvalue-functor/`).

**This plan is the deep fix TUR-E0309 stands in for: actually letting a wide
by-value functor flow through the lens boundary, and deleting the E0309 gate
once it does.** It is the direct continuation of slice **MB2.5** -- which already
solved the *same class of problem for the direct `(f a) -> (f a)` shape* -- into
the lens (nested-fn / curried-result) shape MB2.5 explicitly left out of scope.

### What MB2.5 already solved, and why the lens shape is still open

MB2.5 made a by-value aggregate functor (`Maybe`/`Option`) work as the `(f a)` of
a **direct** constrained rank-2 body `forall f. Functor f => (f int) -> (f int)`
(fixture `hrt-hkt-aggregate-functor-dispatch/`). The insight that made it cheap:
a runtime dict always speaks the carrier ABI, so the aggregate box/unbox already
happens at the **poly-carrier boundary** -- the caller boxes the `(f a)` argument
with `emit_agg_box` and unboxes the `(f b)` result with `emit_agg_unbox`
(`src/compiler/emit_expr.c:2745-2757`, `:2835-2845`), exactly as for a
carrier-compatible functor. Nothing extra had to box at the dispatch site.

The **lens** shape does not go through that one boundary. Its `(f a)` value never
appears as a direct argument or result of the poly call; instead it crosses
**three lens-specific boundaries** that the direct-shape bridge does not cover:

1. **The dict-dispatched method itself.** Inside the lens body,
   `fmap (g (getter s)) setter` dispatches `fmap` through the runtime dict. Its
   `(f a)` argument and `(f b)` return are wide aggregates, but the dict slot
   stores the *carrier* instance method (int64 in / int64 out). MB2.5 keeps the
   dispatch on the carrier and derives the fn-ptr cast's return type from the
   representative instance method (`emit_call_is_dict_param_dispatch`,
   `src/compiler/emit_core.c:1676`) -- but for the lens it never boxes the
   aggregate operands around that call.
2. **The fat-boxed functor-wrapping function `g`.** `g : (-> A (f A))` crosses
   the poly carrier as a uniform fat box; `make_dict_clone` marks it `boxed`
   (`src/compiler/elab_call.c:5538-5550`) so the body fat-dispatches it. When the
   lens body invokes `g`, the `(f A)` it *returns* is a wide aggregate that must
   be unboxed out of the fat-dispatch result.
3. **The lens's own `(f S)` result.** The dict-clone currently forces its result
   to the int64 carrier and refuses to copy a `result_full_type` that mentions
   the type variable (`make_dict_clone`, the `!type_mentions_tyvar` guard at
   `src/compiler/elab_call.c:5559-5561`) -- "a concrete-typed result is left to
   the aggregate-functor (M7) path, out of MB2's scope." That guard is exactly
   the seam this plan opens.

So the lens boundary needs the *same* aggregate box/unbox bridge MB2.5 used, but
threaded across these three additional crossings rather than the single
direct-call boundary. That is **Path A** below. **Path B** (full by-value HKT
monomorphization) removes the carrier round-trip entirely and is the eventual
zero-overhead answer.

## The three fix directions (from the report), scored

The report
([../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md))
lists three directions. This plan commits to a sequence, not a menu:

- **Path A -- extend the aggregate carrier bridge across the lens crossings
  (SHIP FIRST).** Reuse `emit_agg_box`/`emit_agg_unbox` (already load-bearing for
  the direct shape) at the dict-dispatch operands, the fat-`g` result, and the
  lens result. Keeps the uniform-carrier body; pays one heap box + copy + unbox
  per aggregate crossing. Bounded blast radius (mode-B dispatch/clone emit +
  the `make_dict_clone` result gate). This is the report's direction #3
  (auto-box at the boundary) realized as a targeted extension of an existing
  bridge, and the natural continuation of MB2.5's Path A.
- **Path B -- complete by-value HKT monomorphization (GRADUATION).** Re-emit the
  lens body per concrete functor so `(f a)` is spelled by value end to end -- no
  carrier, no box (`g_m7_hkt_enabled` machinery,
  [../guides/monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md)).
  The report's direction #1; the *right* long-term answer; large and cross-cutting
  (elaboration spec-keying, emit spec naming, residual carrier bridges). Deferred
  until a profile shows the Path A box/unbox on a hot path.
- **Path C -- widen the mode-B carrier to two words / pointer-to-payload
  (NON-GOAL).** The report's direction #2. A two-word carrier only rescues
  exactly-two-word functors and still fails three-word ones, while regenerating
  every poly-fn fixture's `expected.c` (the "largest blast radius" the mode-B
  plan chose B1 specifically to avoid). Documented here as a rejected
  alternative, not a slice.

Recommendation: **land Path A behind a flag, retire TUR-E0309 as it lands, and
leave Path B as the documented graduation.** Path A makes wide functors *work*;
Path B makes them *free*.

## Slices

Each slice ships behind a fresh experiment flag (CLAUDE.md experimental-features
rule -- one fully-populated `EXPERIMENTS[]` row, a `g_opt_<name>` bool the
elaboration reads, an `experiment_warn_if_used` call, `expires_at` honored by the
release-cut skills) and adds fixtures before landing. Flags are independent so we
can stop after any slice.

### Slice WF1 -- aggregate `(f a)` across the dict-dispatched method (`--enable=vl-wide-functor`)

**Goal.** Inside a lens body's dict-clone, when the dispatched class method
(`fmap`) has a wide-by-value-aggregate `(f a)` argument and/or `(f b)` return,
box the argument into the carrier before the indirect call and unbox the return
after it -- so the carrier dict slot (which speaks int64) receives and produces
carrier words while the surrounding body sees the by-value aggregate.

**Work.**
- `src/compiler/emit_core.c` -- in the dict-param-dispatch emit
  (`emit_call_is_dict_param_dispatch` path, `:1676`/`:1695`): for each method arg
  whose resolved type `emit_type_is_byvalue_adt` is true, wrap the emitted
  operand in `emit_agg_box`; when the method's own result is such an aggregate,
  wrap the whole dispatch expression in `emit_agg_unbox`. The fn-ptr cast stays
  int64-in/int64-out (it mirrors the carrier dict field, unchanged from MB2.5).
- Reuse the existing `emit_type_is_byvalue_adt` predicate (`emit_expr.c:374`) so
  carrier-compatible functors (opaque / `:heap`) take the untouched
  int64-through path -- byte-identical output for the shipped
  `van-laarhoven-lens-*` fixtures.
- Fixture `van-laarhoven-lens-wide-identity/` -- the `concrete` fixture with
  `Identity` redeclared as `(defstruct Identity :copy [A] (wrapped : A) (tag : int))`
  and its `Functor` instance mapping over `wrapped`; `over`/`set`/`view` return
  the same `3/30/4/99` as the opaque version. (This is precisely the report's
  minimal repro, flipped from an error fixture to a passing one.)

**Risk.** Medium. The box/unbox helpers are proven (MB2.5); the new part is
locating every aggregate operand/return at the dispatch site and not
double-boxing one that already crossed a poly boundary.

### Slice WF2 -- aggregate result out of the fat-boxed functor-wrapping `g` (folded into `--enable=vl-wide-functor`)

**Goal.** When the lens body invokes the fat-boxed `g : (-> A (f A))` and `(f A)`
is a wide aggregate, unbox the fat-dispatch result into the by-value aggregate
the body consumes (it feeds straight into `fmap`).

**Work.**
- `src/compiler/emit_fns.c` -- the dict-clone body emit (the `fd->dict_clone_*`
  sites, `:452`/`:474`/`:678`/`:1323`/`:1396`): where a `boxed` fn param is
  fat-dispatched and its declared result is a by-value aggregate, apply
  `emit_agg_unbox` to the dispatch result. Mirrors the direct-shape result unbox
  (`emit_expr.c:2841`) but keyed on the fat-`g` call rather than the poly call.
- The pass site already boxes a thin `g` via `EX_FN_TO_FAT` after constraint
  pinning (`elab_call.c`, the MB4 note at `:5543`); confirm the aggregate return
  survives that boxing with its full type intact so the unbox resolves the right
  `cn`.
- Fixture extends WF1: `set`/`over` use a *capturing* `g`
  (`\a -> Identity (h a)`), so a wrong dispatch or a missing unbox crashes rather
  than silently passing -- the same crash-hardening the MB4 fixtures rely on.

**Risk.** Medium. Sourcing the aggregate type from a fat-dispatched param return
is the genuinely new emit site; contained by gating on `emit_type_is_byvalue_adt`
of the param's result type.

### Slice WF3 -- lens `(f S)` result: open the `make_dict_clone` type-var gate (folded into `--enable=vl-wide-functor`)

**Goal.** Let the dict-clone carry a `result_full_type` that mentions the functor
variable (`(f S)`) when the instantiating functor is a wide aggregate, and box
the clone's aggregate return into the carrier so the poly-carrier boundary's
existing `emit_agg_unbox` (`emit_expr.c:2841`) recovers it -- the lens's result
is a normal poly-call result once the clone returns the carrier.

**Work.**
- `src/compiler/elab_call.c` `make_dict_clone` -- replace the blanket
  `!type_mentions_tyvar(result_full_type)` refusal (`:5559-5561`) with: keep the
  carrier `result_kind`, but record that the clone's *natural* result is a
  by-value aggregate (a new `FnDef.dict_clone_boxes_result` flag or by reusing
  the existing `poly_wrap boxes_aggregate` seam, `expr.h:945`) so emit boxes the
  return.
- `src/compiler/emit_fns.c` -- when `dict_clone_boxes_result`, the three
  return-type sites (signature, `ret_ctype`, the return statement) emit the int64
  carrier and wrap the returned aggregate in `emit_agg_box` (the inverse of
  MB2.5's "return the dispatch result directly" -- there the result was already a
  carrier word; here it is a by-value aggregate that must be boxed first).
- `src/compiler/emit_module.c` -- mirror the carrier return in the clone's
  forward declaration (MB2.5 did the same for the carrier-return case,
  `:emit_module` forward-decl mirror).
- **Delete TUR-E0309** from `elab_poly_call` (`elab_call.c:~5985`) once WF1-WF3
  are on: a wide-by-value functor pinned through the lens shape is now supported,
  so the gate that rejected it is dead. Convert
  `errors/van-laarhoven-wide-byvalue-functor/` into a passing fixture (or delete
  it in favor of `van-laarhoven-lens-wide-identity/`). Until the flag graduates,
  keep TUR-E0309 firing when `--enable=vl-wide-functor` is **off** (so the
  default path still gets a clean error, never a segfault); gate the error on
  `!g_opt_vl_wide_functor`.

**Risk.** Medium-high. This reopens the exact `result_full_type` seam MB2 closed
deliberately; the guard exists because a clone whose result mentions the tyvar
was previously flagged generic-unsafe and skipped. The fix is to box the result
so the wrapper is generic-safe again (the return is a carrier word), not to leave
the tyvar in the ABI. Contained by the flag and by gating every new branch on
`emit_type_is_byvalue_adt`.

### Slice WF4 -- acceptance: wide functors in the four VL shapes + guide note (folded into `--enable=vl-wide-functor`)

**Goal.** Prove the wide functor works across the same four shapes the
carrier-compatible functor already passes, and document the (now-removed)
restriction as history.

**Work.**
- Fixtures mirroring the shipped set with a wide `:copy` functor:
  `van-laarhoven-lens-wide-{concrete,generic,compose}/` -- `view`/`set`/`over`
  and composition, same numeric expectations as their opaque twins.
- A mixed fixture: a lens composed from one carrier-compatible and one wide
  functor stage, proving the two coexist through one compiled `view`/`set`/`over`.
- Update `docs/guides/lens-guide.md:95-98`: drop "A functor whose `(f a)` is a
  wider by-value aggregate is still the mode-B No-go"; state plainly that any
  `Functor` instance works, with a one-line note that a `:copy`-struct functor
  pays a heap box/unbox per crossing until Path B lands (link this plan).
- Move the report to `docs/archive/` (per CLAUDE.md STRICT archiving rule) once
  the flag is default-on / graduated.

**Risk.** Low. Fixtures + docs; the machinery is WF1-WF3.

## Non-goals

- **Path C (wider carrier).** Rejected above -- helps only two-word functors,
  regenerates every poly-fn snapshot.
- **Path B (full by-value HKT monomorphization).** The zero-overhead graduation;
  its own effort, tracked by `g_m7_hkt_enabled` and the monomorphization guides.
  This plan makes wide functors *correct*, not *free*.
- **Impredicative use / storing a constrained `forall` in a container** -- still
  out (would force carrier option B2 from the mode-B plan).
- **Non-`Lens'` optics** (Prism/Traversal) -- the gate stays `Lens'`, as in the
  parent plans.

## Cost estimate (rough)

| Slice | Surface | Type system | Codegen | Tests/docs | Risk |
| --- | --- | --- | --- | --- | --- |
| WF1 -- dispatch operand box/unbox | small | none | small (reuse bridge) | small | medium |
| WF2 -- fat-`g` result unbox | small | none | small | small | medium |
| WF3 -- lens result gate + box | small | small (clone flag) | medium | small | medium-high |
| WF4 -- acceptance + guide | none | none | none | medium | low |

The whole of Path A is smaller than MB2 was: no new dispatch node, no carrier
widening, no fixture-wide `expected.c` regen -- it threads an *existing* bridge
(`emit_agg_box`/`emit_agg_unbox`) across three already-identified crossings and
opens one deliberately-closed result gate.

## Open questions

1. **Double-box avoidance.** A `(f a)` that crosses BOTH the fat-`g` boundary
   (WF2) and immediately the dict-dispatch boundary (WF1) must box/unbox once,
   not twice. Confirm the box happens at the outermost crossing and the inner
   sees the already-boxed carrier (or that the unbox/rebox cancels cleanly).
2. **Drop glue.** A wide `:copy` functor with a `needs_drop_glue` field (an
   `rc`/`ref` payload) heap-boxed at a crossing must free the box without
   double-dropping the payload. WF1-WF3 should restrict to drop-glue-free
   functors first (matching `adt_field_is_inline_byval_d`'s conservatism) and
   file a follow-up for glue-bearing ones.
3. **`Const r` with a wide `r`.** `view` instantiates `f := Const r`. A
   carrier-compatible `Const` holds its `r` boxed in one word regardless of
   `r`'s width, so `view` should stay on the untouched path even when the *focus*
   is wide -- verify the WF gating keys on the *functor* layout, not the focus.
4. **When does Path B become worth it?** Name the profile signal (box/unbox
   showing up in a lens-heavy hot loop) that promotes Path B from deferred to
   scheduled, so the deferral is a decision with a trigger, not an indefinite
   punt.

## Related

- [../reported/van-laarhoven-functor-must-be-int-carrier.md](../../reported/van-laarhoven-functor-must-be-int-carrier.md)
  -- the tracked gap and the TUR-E0309 diagnostic this plan retires.
- [`constrained-hkt-forall-mode-b-plan.md`](constrained-hkt-forall-mode-b-plan.md)
  -- MB1-MB4 (dict passing/dispatch, lens) and **MB2.5** (the direct-shape
  aggregate bridge this plan generalizes to the lens boundary).
- [../guides/lens-guide.md](../../guides/lens-guide.md) -- the shipped lens guide
  (the restriction note WF4 removes).
- [../guides/monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md)
  -- the `g_m7_hkt_enabled` by-value-HKT machinery Path B completes.
- `src/compiler/emit_expr.c:440-460` -- `emit_agg_box`/`emit_agg_unbox`, the
  bridge WF1-WF3 thread across the lens crossings.
- `src/compiler/emit_expr.c:2745-2757`, `:2835-2845` -- the direct-shape
  poly-carrier box/unbox WF1/WF3 mirror.
- `src/compiler/emit_core.c:1676` -- `emit_call_is_dict_param_dispatch`, the WF1
  edit site.
- `src/compiler/elab_call.c:5503` -- `make_dict_clone` and the
  `!type_mentions_tyvar` result gate WF3 opens.
