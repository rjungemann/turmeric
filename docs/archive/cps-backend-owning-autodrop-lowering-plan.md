---
title: CPS backend -- lower the NON-ref owning-value scope-exit auto-drop (unblocks E2)
category: Planning
status: LANDED / RESOLVED 2026-07-21 -- P1 + P2 + P3 all landed. P1 (non-crossing hoist) and P2 (single-shot crossing lowered in place -- unblocked Track B E2) landed earlier; P3 (multi-shot crossing) landed with E3's graduation 2026-07-20 (the `owning-cloneable-capture` experiment retired, `g_opt_owning_cloneable_capture` defaults true), which threaded the auto scope-exit drop `(defer (rc/drop r))` across a cloneable-reset (E3b auto-defer) and fires the region-end teardown on abortive control. Fixture `cloneable-owning-autodrop-crossing` (output 32, leak-clean) covers the P3 case. The missing generalization of O1-b, now complete for every reachable shape.
description: A colored (CPS-emitted) function that carries an elaborator-injected scope-exit auto-drop for an owning value EVICTS to the whole-function fallback, because `EX_DEFER` has no CT-IR lowering. O1-b P1 lowered exactly ONE shape of this -- `(defer (drop! r))` on a bare `ref<T>` var. Every other owning auto-drop is untouched and still evicts: a bare `rc` relying on auto-drop (`(defer (rc/drop r))`), and -- the case that blocks Track B E2 -- a by-value struct/record local with owning fields, whose fix (byvalue-struct-local-owning-field-leak, RESOLVED on the direct path) injects `(defer (rc/drop (.f o)))` / `(defer (drop! (.f o)))` per owning field. This plan generalizes O1-b's auto-drop recognizer + hoist to those shapes. Crucially it corrects an earlier misconception: E2's BORROW captures need this lowering landing in a SINGLE-SHOT continuation, which is sound WITHOUT the E3 teardown -- only genuinely-consuming / abortive crossings ride E3. Sound on the fallback today; missed coverage, not a correctness gap.
---

# CPS backend -- lower the non-ref owning-value scope-exit auto-drop

> **Progress note (2026-07-21) -- FULLY LANDED, archiving.** Re-verified against
> the tree. **P1 + P2 + P3 are all landed.** The generalized recognizer
> `autodrop_defer_owning` (+ `autodrop_root_local`) is in
> `src/passes/cps_ir.c:2221-2262`; the P2 soundness gate
> `expr_has_unsafe_control` (`:2349`) is present and recurses through
> handle/perform/reset arms as described. **P3 landed with E3's graduation
> (2026-07-20):** the earlier note said P3 was gated on an unbuilt E3
> DK-teardown, but E3 has since GRADUATED
> ([cps-backend-owning-env-teardown-e3-plan.md](../archive/cps-backend-owning-env-teardown-e3-plan.md),
> now in the archive) -- the `owning-cloneable-capture` experiment retired and
> `g_opt_owning_cloneable_capture` defaults true. E3b's auto-defer lowering
> threads the elaborator's `(defer (rc/drop r))` across a cloneable-reset (fires
> once after the region completes, and the region-end teardown fires on abortive
> control too), so the P3 owning-autodrop-crossing-a-cloneable-reset case is now
> lowered rather than evicted. Fixtures verified leak-clean under the harness
> (leak detection ON): `cps-backend-owning-autodrop-noncrossing` (P1),
> `cps-backend-owning-autodrop-crossing-singleshot` (P2), the E2 capstone
> `cps-backend-owning-struct-capture-multishot` (P2), and
> `cloneable-owning-autodrop-crossing` (P3, output 32) -- plus the full
> `cloneable-owning-*` family (19 fixtures, 0 failed). This plan is
> **RESOLVED**; nothing reachable remains open.

## Why this document exists (and why it was missed)

The env-capture / Track B plans repeatedly hand-waved this as *"the `EX_DEFER`
lowering (O1-b / a sibling item)"* -- but that sibling **did not exist**. O1-b
([cps-backend-ref-scope-exit-drop-plan.md](../upcoming/cps-backend-ref-scope-exit-drop-plan.md))
is deliberately `ref<T>`-only. And the very change that RESOLVED the direct-path
by-value-struct leak
([docs/archive/byvalue-struct-local-owning-field-leak.md](../archive/byvalue-struct-local-owning-field-leak.md))
is what planted the CPS eviction: it started injecting a scope-exit auto-drop for
by-value struct owning fields, and that injected `EX_DEFER` has no CT-IR
lowering, so every colored function carrying such a local now evicts. The report
that made the change is archived and only ever spoke of the direct path -- it
even notes "not currently tracked anywhere" for the CPS side. So the regression
in *coverage* slipped through across many commits precisely because no plan owned
the non-`ref` `EX_DEFER` lowering. **This document is that plan.**

**Nothing here is a correctness gap today.** Every shape below is handled
correctly by the whole-function fallback (the direct emitter fires the defer
through the `tur_frame` LIFO stack). This is coverage -- and it is the concrete
unblock for Track B E2 in
[cps-backend-multishot-continuations-owning-capture-plan.md](../archive/cps-backend-multishot-continuations-owning-capture-plan.md).

## The mechanism today (verified)

### The auto-drop injections (elaboration)

For an owning local not otherwise consumed, `elab_let` injects a scope-exit
discharge (`src/compiler/elab_forms.c`):

- **bare `ref` / `weak` / `lref`**: `(defer (drop! r))` -- one defer, `drop!` on
  the bare var (`~:1024-1070`).
- **bare `rc`** (and constrained existentials): `(defer (rc/drop r))` -- one
  defer, `EX_RC_DROP` on the bare var (`~:1117-1178`).
- **by-value struct/record with owning fields** (`needs_drop_glue`, non-`:heap`,
  detected by `elab_byval_drop_adt`): **one defer per owning field** --
  `(defer (rc/drop (.f o)))` for an rc field, `(defer (drop! (.f o)))` for a
  ref/lref field, where `(.f o)` is an `EX_GET_FIELD` off the by-value local
  (`:1192-1340`). Guarded by the same moved/consumed filters.

All lower to `EX_DEFER` in the elaborated tree. In an **uncolored** function the
direct path registers each defer with the `tur_frame` LIFO stack and fires it at
scope exit / early return / unwind. In a **colored** function there is no such
path: `EX_DEFER` has no CT-IR lowering case (`src/passes/cps_ir.c`), so a colored
function still carrying any of these evicts (`[EVICT] BODY-UNSUPPORTED ...
unsupported form: EX_DEFER`).

### What O1-b P1 lowered, and why it is too narrow

O1-b P1 added `plan_autodrop` (`src/passes/cps_ir.c:1441`): recognize a `do`
block ending in trailing auto-drop defers, find the first control barrier
(`item_has_control`), verify **non-crossing** (no owning var referenced at/after
the barrier, via `expr_refs_binding`), and emit each drop straight-line BEFORE
the barrier (`cps_emit_hoisted_drops` -> `build_letraw` -> `CT_LETRAW`, delegated
to the direct emitter exactly like an explicit `rc/drop` / O1-a). `EX_REF` joins
`safe_to_delegate` so the whole function can leave the fallback path.

The recognizer `autodrop_defer_ref` (`:1261`) is the choke point -- it admits
**only**:

- `body->kind == EX_BUILTIN` with `sp->shape == BS_PREFIX_UNARY_FREE` (i.e.
  `drop!`, NOT `EX_RC_DROP`), **and**
- `arg->kind == EX_VAR` (a bare var, NOT a field read), **and**
- `arg->as.var.binding->type.kind == TY_REF` (NOT `TY_RC`, NOT an aggregate).

So `(defer (rc/drop r))` (bare rc), `(defer (rc/drop (.f o)))` and
`(defer (drop! (.f o)))` (struct fields) all return NULL -> `plan_autodrop`
bails -> the defers stay `EX_DEFER` -> evict. **Verified**: a bare `rc` relying
on auto-drop, and a by-value struct with an `rc` field, both evict
`BODY-UNSUPPORTED ... EX_DEFER`.

## Case analysis (mirrors O1-b, generalized)

Let `X` be the owning value the auto-drop discharges (a bare `rc`/`ref` var, or a
by-value local's owning field `(.f o)`), and `C` a control op in `X`'s scope.

1. **`X` does not cross `C`** (created + last-used before `C`, and not captured
   by the continuation). Hoist the drop to before `C` -- exactly O1-b P1, just a
   wider recognizer. **P1 here.**
2. **`X` crosses `C`, `C` is SINGLE-SHOT** (a handle / reset / perform
   continuation, resumed at most once). The value is captured into that
   continuation's env (borrow-only -> E-borrow bare alias) and its drop fires
   once in the (single-shot) continuation. This is exactly how E1's explicit
   `(rc/drop r)` after a `handle` already works. Lower the auto-drop **in place**
   (leave it where it sits -- at scope exit, i.e. in the post-`C` continuation --
   and delegate its body via `CT_LETRAW`), gated so the crossed control op is
   provably single-shot (`owning_dropped_before_control`-style). **P2 here --
   this is what unblocks Track B E2's borrow captures, and it needs NO E3.**
3. **`X` crosses `C`, `C` is ABORTIVE or MULTI-SHOT.** An abortive `shift`
   discards the continuation (drop lost -> leak); a multi-shot resume runs it per
   resume (double free). The drop must fire once per continuation *lifetime* via
   a teardown. **P3 here -- rides E3**
   (the DK-teardown design lives in Track B's E3 section of
   [cps-backend-multishot-continuations-owning-capture-plan.md](../archive/cps-backend-multishot-continuations-owning-capture-plan.md);
   the standalone E3 plan was folded there and deleted)
   and O1-b P2/P3.

The key correction to the earlier plans: **E2 is case 2, not case 3.** E2's
owning-aggregate capture is borrow-only (the case reads a field; the enclosing fn
drops it once, in the single-shot post-handle continuation). So E2 rides **P2
here**, not E3. E3 is only for the genuinely-consuming / abortive crossings.

## Design

### The core change: generalize the recognizer

Replace `autodrop_defer_ref` with `autodrop_defer_owning(e)` returning the
discharged owning value's *root local* binding (for the crossing check) plus the
delegatable drop body, admitting the elaborator-injected owning auto-drop shapes
and NOTHING else (a general user `defer` must still fall back):

- `(defer (drop! X))` where `X` is a bare `ref`/`weak`/`lref` var **or** a
  field read `(.f o)` off a by-value local -- `EX_BUILTIN` /
  `BS_PREFIX_UNARY_FREE`.
- `(defer (rc/drop X))` where `X` is a bare `rc` var **or** a field read
  `(.f o)` -- `EX_RC_DROP`.

Root local = the `EX_VAR` binding, reached through an `EX_GET_FIELD`
(`.struct_expr`) / ascribe peel. The crossing check (`expr_refs_binding`) keys on
that root local, so a field-drop of `o` counts `o` as the crossed value.

`plan_autodrop` and `cps_emit_hoisted_drops` are otherwise reusable as-is: they
already collect trailing defers, locate the barrier, and delegate each drop body
via `build_letraw` / `CT_LETRAW` -- and `CT_LETRAW` already lowers `rc/drop`, a
field-read arg, and `drop!` on the direct side (O1-a). The delegated constructors
(`EX_MAKE_STRUCT` for the struct local; `EX_RC_OF` etc.) already pass
`safe_to_delegate`; confirm `EX_GET_FIELD` inside the drop body does too.

### Phasing

- **P1 -- non-crossing (self-contained, no runtime). LANDED.** Widened the
  recognizer: `autodrop_defer_ref` -> `autodrop_defer_owning` (`src/passes/cps_ir.c`)
  now admits `(defer (rc/drop X))` and `(defer (drop! X))` where `X` is a bare
  owning var OR a field read `(.f o)` (rooted at the by-value local via
  `autodrop_root_local` for the crossing check), with an owning-type guard so a
  non-drop free-shape builtin is never matched. `plan_autodrop` /
  `cps_emit_hoisted_drops` were reusable as-is (they already delegate each drop
  body via `CT_LETRAW`, and `safe_to_delegate` already admits `make-struct` /
  `rc/of` / field reads). Fixture `cps-backend-owning-autodrop-noncrossing`: a
  colored fn binds a bare `rc` and a by-value struct with an `rc` field, uses
  each before a `shift`, and delivers through a `reset`; both auto-drops hoist,
  the fn CPS-emits, `direct == cps`, LeakSanitizer-clean. Verified the boundaries
  hold: a value used at/after the control op still evicts (crossing -> P2/P3), and
  a user `(defer (println ...))` of arbitrary effects is never hoisted. Full
  suite 2167 passed, 0 failed (no snapshot churn).

- **P2 -- crossing into a single-shot continuation. LANDED (unblocks E2).** When
  the value's auto-drop crosses a control op whose post-op continuation is
  SINGLE-SHOT, `plan_autodrop` no longer bails -- it routes to the existing
  drops-at-exit branch (`pl->first_ctrl = n_real`), lowering the drop in place, in
  the single-shot continuation, where it fires once (exactly how E1's explicit
  `(rc/drop r)` after a `handle` works). *Soundness gate*: a self-contained
  `expr_has_unsafe_control` (`src/passes/cps_ir.c`) -- true iff a real item
  contains an abortive / delimited / multishot / escape op (`shift`/`shift0`/
  `reset`/`cloneable`/`serial`/`callcc`/`discontinue`/`async`/`await`), recursing
  into `handle`/`perform`/`resume` bodies, conservative-true on un-modelled nodes.
  If any crossed item is unsafe, fall back (P3 / E3).

  Combined with the re-landed Track B E2 `cap_owning_ok` extension (carrier ADT +
  `owning_byvalue_aggregate`) and `expr_is_pure_borrow_of` recognizing a
  NON-owning field read `(.f o)` as a pure borrow, this makes an owning-aggregate
  captured borrow-only into a handler case CPS-emit, leak-clean, WITHOUT E3.
  Fixtures: `cps-backend-owning-struct-capture-multishot` (a `defstruct` with an
  `rc` field, captured into a case that RUNS TWICE via a two-perform `g`, reads
  `.tag`; output 18, leak-clean) and `cps-backend-owning-autodrop-crossing-singleshot`
  (bare rc + struct used after a handle, auto-dropped in the post-handle
  continuation; output 110). Boundaries verified: a consuming aggregate case, an
  owning-field read `(.r o)`, and an abortive-shift crossing all still fall back.
  Full suite 2168 passed, 0 failed.

- **P3 -- crossing into an abortive / multi-shot continuation. LANDED (with E3
  graduation, 2026-07-20).** Rode E3's DK-teardown (fire the drop once per
  continuation lifetime, and on abortive abandon). E3 graduated -- the
  `owning-cloneable-capture` experiment retired, `g_opt_owning_cloneable_capture`
  defaults true -- and E3b's auto-defer lowering threads the elaborator's
  `(defer (rc/drop r))` / `(defer (drop! r))` across a cloneable-reset via the
  `cps_tail` `do`-lowering's P5b trailing-defer extension, lowering it to the
  same straight-line `CT_LETRAW` drop the explicit channel emits (no full "CPS
  defer runtime" needed). The region-end teardown fires on abortive control too.
  Fixture `cloneable-owning-autodrop-crossing` (an owning `rc` captured `^borrow`
  across a two-resume cloneable-reset, auto-dropped at exit; output 32,
  leak-clean, no hand-written drop). Same substrate O1-b P2/P3 land on. The one
  residual bounded by the base language (a consuming `ref<owning>` /
  nested-aggregate field) is a clean hard error, not a miscompile, and requires
  deepening the base SHALLOW drop-glue -- out of scope here.

## Interaction with other plans

- **Track B E2** (multishot plan): unblocked by **P2 here** (single-shot borrow
  capture) plus E2's `cap_owning_ok` extension -- NOT by E3, correcting the
  earlier "E2 rides E3" note. Update the multishot plan's E2 section to point
  here.
- **O1-b** (ref scope-exit drop): this is the non-`ref` generalization of the
  same mechanism. Ideally `autodrop_defer_ref` becomes `autodrop_defer_owning`
  and O1-b P1's `ref` case folds into P1 here (one recognizer, one hoist). O1-b
  P2/P3 and P3 here share E3.
- **E3** (Track B's E3 section of
  [cps-backend-multishot-continuations-owning-capture-plan.md](../archive/cps-backend-multishot-continuations-owning-capture-plan.md);
  folded from the deleted standalone plan): only P3 (abortive / multi-shot
  crossing) depends on it. P1/P2 are independent and land first.
- **byvalue-struct-local-owning-field-leak** (archived, RESOLVED on direct):
  its injection is the source of the struct-field defers; this plan lowers them
  under CPS. Add a back-reference so the CPS side is no longer "not tracked."
- **N6.5** (fallback deletion): a colored fn with any owning auto-drop is a
  residual fallback case; P1/P2 remove the non-crossing + single-shot-crossing
  subsets, P3/E3 the rest. List when N6.5 audits residuals.

## Depends on / reuses

- The auto-drop injections: `elab_let` (`src/compiler/elab_forms.c:~1024-1340`;
  `elab_byval_drop_adt` at the file head).
- O1-b P1 machinery to widen: `autodrop_defer_ref` / `autodrop_defer_body`
  (`src/passes/cps_ir.c:1261-1279`), `plan_autodrop` (`:1441`),
  `cps_emit_hoisted_drops` (`:1495`), `item_has_control` (`:1289`),
  `expr_refs_binding`, `safe_to_delegate` (`:929`).
- Single-shot judgement for P2: `owning_dropped_before_control` / `letraw_ok`
  (`src/compiler/emit_cps_ir.c`).
- `CT_LETRAW` drop delegation (O1-a) as the emit shape.
- Track B E2 `cap_owning_ok` extension + `owning_byvalue_aggregate` (prototyped,
  reverted; multishot plan E2 section) for the capture side of the P2 fixture.

## Out of scope

- **General user `defer`** in a colored function -- arbitrary deferred effects;
  the recognizer stays gated to the elaborator-injected owning auto-drop shapes.
- **Heap ADT / carrier handles** -- verify whether they carry a similar scope-exit
  auto-drop (they may use the control-block `drop_fn` path instead). If they do,
  they ride the same P1/P2/P3; if not, they are a separate item. Confirm before
  claiming them here.
- **`weak` field decrement** -- the byvalue injection intentionally leaves `weak`
  fields to leak their weak count (no scope-exit weak-decrement primitive); out
  of scope here too.
- **The env teardown itself** -- E3 owns it; P3 lands on it.
