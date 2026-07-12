---
title: Owning pointers on the CPS-IR-to-C backend -- analysis + plan
category: Planning
description: Graduation gate item 4 for the cps-backend experiment asks that owning pointer types (ref / rc / weak / lref) thread through the DK continuation slot "with correct drop / refcount / linearity discipline, not merely bit-copied." This document records what the investigation actually found -- bare owning pointers are not expressible as function results or continuation payloads in the Turmeric source language, so they never cross the one-word DK slot as bare values; they cross only as fields of structs / ADTs, which makes their slot discipline a subset of the N3 struct/ADT work rather than a standalone slot_ty change -- and lays out the small CPS-specific remainder (drop-node translation) with the multi-shot analysis that de-risks it.
---

# Owning pointers on the CPS-IR-to-C backend -- analysis + plan

## Why this exists

The `cps-backend` graduation gate
([cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md#graduation-gate----what-must-hold-before-cps-backend-goes-always-on),
item 4) asks that owning pointer types -- `ref<T>` (owning, auto-drop),
`rc<T>` (refcounted), `weak<T>` (weak-refcounted), `lref<T>` (linear) -- thread
through the one-word DK slot "with correct drop / refcount / linearity
discipline ... not merely bit-copied." The N1 status of the non-scalar plan
deferred them pending "an ownership hook" on the slot convention.

The investigation for this document found that the framing above is **not the
shape of the actual work**. This doc records the findings and re-scopes item 4.

## Finding 1 -- owning pointers are never bare slot values

A bare owning pointer cannot be a function return type or a bare continuation
payload in the Turmeric source language. All four spellings are a hard type
error in return position:

```turmeric
(defn mk [] : rc<int>   (rc/of 42))   ; error: unsupported return type keyword 'rc<int>'
(defn mk [] : ref<int>  ...)          ; error: unsupported return type keyword 'ref<int>'
(defn mk [] : lref<int> ...)          ; error: unsupported return type keyword 'lref<int>'
(defn mk [] : weak<int> ...)          ; error: unsupported return type keyword 'weak<int>'
```

Owning pointers live only as **fields of structs / ADTs** (`rc<Inner>` inside a
`Box`, `next : rc<S>` in a linked node, etc.) -- every occurrence in the fixture
corpus is a field type, never a bare result. Since the DK slot only ever carries
a function result / continuation payload, **a bare owning pointer never crosses
the slot.** What crosses is the enclosing struct / ADT.

Consequence: there is no `slot_ty` row to add for `ref` / `rc` / `weak` /
`lref`. Adding them would type surface that the source language cannot even
produce.

## Finding 2 -- the real crossing is the aggregate (folds into N3)

An owning value reaches a continuation only inside a struct / ADT. Those cross
the slot as either a carrier-ABI handle (Tier A carrier -- `type_uses_carrier_abi`)
or a by-value aggregate (Tier C -- `adt_is_byvalue_product`), both of which are
**N3** in the non-scalar plan. The ownership discipline for an owning field is
the **enclosing aggregate's drop glue** (`rc_strong_decrement` on the field, the
struct's generated drop function), not a per-pointer slot hook.

So item 4 is a **subset of N3**, not a standalone phase: when N3 lets a
carrier / aggregate ADT ride the slot, it must ensure a value *containing* an
owning field gets its drop glue run on the CPS path exactly once (see Finding 4).

## Finding 3 -- multi-shot is already de-risked by the zero-capture cut

`dk_invoke` resumes a continuation by `dk_copy` -- a **shallow, multi-shot** copy
of the DK node chain (`src/runtime/cps_prompt.c`). A shallow copy of a frame
whose `env` held an owning pointer would duplicate the pointer bits with no
incref, so invoking the continuation twice would drop it twice -> double free.

That hazard is **already excluded** by the backend's existing zero-capture
restriction: `emit_cps_ir.c` only lowers a `reset` / `shift` / `handle` whose
continuation captures nothing (`has_capture` / `has_capture_case` guards). A
captured owning value is exactly a non-empty capture, so such a function stays
on the fallback path today. The multi-shot double-free-via-capture cannot arise
in the current CPS subset.

Slot-*delivered* owning values (the `w` argument of `dk_run` / `dk_invoke`) are
a different thing: each invocation delivers a fresh value that the continuation
consumes once, so delivery is a single-consumption **move** -- bit-copy is
correct for a move, no incref needed. (A source that resumes the *same* owning
value twice would be a linearity error the front end already rejects, or would
require an explicit `rc/clone`.)

## Finding 4 -- the one genuine CPS-specific remainder: drop-node translation

Auto-drop is injected during **elaboration**, not emission: a `ref<T>` bound in
a `let` gets a `(defer (drop! r))` node appended at scope exit
(`elab_forms.c` ~1000), gated on the front end's move analysis
(`is_moved` / `is_linear_consumed` / `is_binding_consumed`), so a moved value's
drop is already elided. `rc` drops are explicit `rc/drop` (`EX_RC_DROP`) or the
struct drop glue.

`cps_ir.c` does not translate `EX_DEFER` (nor `EX_RC_DROP`), so today a colored
function that binds an owning value locally -- even one that never crosses the
slot -- collapses to `CT_UNSUPPORTED` and **falls back** (verified: a `reset`
body binding an `rc` and auto-dropping it stays off the CPS path, returns the
correct value, and is LeakSanitizer-clean). That is safe but leaves the CPS path
narrower than it needs to be.

The genuine remainder is therefore: **translate the drop nodes** so a colored
function containing a locally-bound owning value (or, post-N3, an owning-field
aggregate) can be CPS-emitted, with the drop run exactly once on the straight-line
path. Because the front end already places (and elides) the drops correctly, this
is a faithful lowering of existing `EX_DEFER (drop! r)` / `EX_RC_DROP` nodes to
CPS-IR nodes plus their emission, not a fresh ownership analysis.

## Current state is safe

Every owning-pointer situation the source language can express **falls back
today**, and the fallback (fiber) path handles ownership correctly:

- Bare owning result / payload: not expressible (Finding 1).
- Owning value inside a struct / ADT crossing the slot: the aggregate is not in
  `slot_ty` (N3 not landed), so it falls back.
- Owning value bound locally in a colored function: its `EX_DEFER` / `EX_RC_DROP`
  is `CT_UNSUPPORTED`, so it falls back (Finding 4).
- Captured owning value in a continuation: excluded by the zero-capture cut
  (Finding 3).

No naive bit-copy of an owning pointer through the slot occurs. There is no
open memory-safety bug here -- only missed CPS coverage.

## Plan

### O1 -- owning-value locals via delegated emission -- **LANDED**

Refined during implementation. Investigation showed the owning-value operations
are **distinct `Expr` kinds** (`EX_RC_OF` / `EX_RC_CLONE` / `EX_RC_DROP` /
`EX_RC_COUNT` / `EX_RC_PTR`), not generic builtins, and that supporting a local
`rc` needs the whole set (a lone drop unblocks nothing -- the producer/reader
fall back too). Rather than re-implement the control-block / refcount / drop-glue
emission in the CPS backend (duplication + drift risk), the backend **delegates**:

- New `CT_LETRAW` IR node (`cps_ir.h`) carries the source `Expr`. `cps_ir.c`
  translates each owning-value op to `CT_LETRAW` **only when its operand is
  atomic** (`is_delegatable_owning`), so no control operator can hide in an
  operand and get emitted in direct style inside a CPS function.
- `emit_cps_ir.c` emits `CT_LETRAW` by calling the direct emitter,
  `emit_value(ctx, out, e)` -- reusing the exact discipline the direct path uses,
  correct by construction. The owning value stays a **local** and never crosses a
  DK slot, so `slot_ty` still (correctly) excludes the owning types.
- Naming: a `CT_LETRAW` binder that stands for a source `let` name is declared and
  assigned via `name_for_binding` (the `CVar` now carries its source `Binding`),
  matching every reference site -- including the references `emit_value` itself
  emits for a later `rc/drop`. A nil-typed op (`rc/drop`) binds the unit
  placeholder rather than assigning its void result.
- Move elision is the front end's: only the non-elided `rc/clone` / `rc/drop`
  nodes are in the tree, so faithfully emitting "the ops that are there" is
  correct. The zero-capture cut keeps the owning value out of any multi-shot
  continuation (a captured owning local makes the `reset`/`shift` fall back).
- **Liveness-across-control guard (found during testing -- essential).** An
  abortive `shift` *discards its continuation*: in the CPS IR the post-shift code
  is not present. If an owning value is allocated before such a shift and its
  balancing drop lives in that discarded continuation, the drop vanishes (leak)
  and the value aborts past the code that used it (a real miscompile -- observed:
  an `rc` used after a shift returned the shift value, not the computed one). The
  guard `owning_dropped_before_control` in `term_core_ok` requires an allocating
  op (`rc/of` / `rc/clone`) to be balanced by its `rc/drop` on a **straight-line
  path before any control op, branch, or delivery**; otherwise the function falls
  back. This is what makes the landed slice sound. The same guard also covers the
  `perform`/`resume` multi-shot case (a control op before the drop -> fall back).
- Fixture: `tests/fixtures/cps-backend-rc-drop/` -- a colored function creates an
  `rc`, clones it, drops both, and delivers a scalar through a `shift`;
  direct-vs-CPS equal (7) and LeakSanitizer-clean (one `rc_cb_alloc`, two
  `rc_strong_decrement`, freed exactly once).

**O1 tail -- cross-branch drops: LANDED.** The liveness guard
(`owning_dropped_before_control`) now descends `CT_IF` (dropped-before-control iff
dropped on *both* arms) and `CT_LETCONT` (execution proceeds in the body; the
join's jbody is reached via an appcont, i.e. after the drop point). So an `rc`
dropped in each arm of an `if` before a following `shift` CPS-emits (fixture
`cps-backend-rc-drop-branch`, LeakSanitizer-clean). Dropping on only one arm is a
front-end linearity error, so it never reaches the backend.

**O1 tail -- `ref<T>` auto-drop (`EX_DEFER`): shown to be low-value, deferred.**
Investigation found that `ref<T>`'s auto-drop is injected as `(defer (drop! r))`
at **scope exit**, which in a colored function is *after* its control op. An
abortive `shift` discards its continuation, so the scope-exit drop lands in the
discarded region -- exactly the case the liveness guard rejects. A `ref` whose
scope ends *before* the control op could work, but then the value it produced is
typically captured by the continuation (fallback). So a `ref` local in a colored
function almost always falls back regardless of `EX_DEFER` translation; the payoff
does not justify implementing scope-exit defer semantics (LIFO, early-return) in
the CPS backend now. Documented; `ref`/`weak`/`lref` locals stay on the fallback.

**Still open in O1 (low priority):** non-atomic owning-op operands (e.g.
`(rc/of (compute))`) still fall back -- delegating them needs a sound "operand
has no control op" scan. Safe fallback meanwhile.

### O2 -- owning fields inside carrier / aggregate ADTs (rides N3)

- When N3 admits a carrier-ABI / by-value ADT to the slot, ensure a value that
  contains an owning field runs the enclosing aggregate's drop glue exactly once
  on the CPS path (the same generated struct-drop function the direct path calls).
- This is a **prerequisite coupled to N3**, not a separate slot mechanism: the
  owning field never rides the slot alone; the aggregate does.

### O3 -- captured owning values (only if the zero-capture cut is lifted)

Deferred with the broader env-capture story (out of scope in the parent plans).
If a future phase lets a continuation capture live locals, an owning capture must
be deep-cloned / incref'd per `dk_copy` (multi-shot). Until then the zero-capture
cut keeps this unreachable, so O3 is not on the critical path for graduation as
long as the cut stands.

## Re-scoping graduation gate item 4

Item 4 should read: **owning pointers are handled correctly on the CPS path** --
which, given Findings 1-4, means:

1. O1 (owning-value locals) -- **landed for explicit `rc` ops with atomic
   operands** (a colored function with a local `rc` CPS-emits with its drop run
   exactly once, LeakSanitizer-clean). Remaining O1 tail: `EX_DEFER` auto-drops
   (`ref<T>`) and non-atomic operands;
2. O2 has landed *as part of N3*, so an aggregate containing an owning field runs
   its drop glue exactly once when it rides the slot;
3. the zero-capture cut still guards O3, or O3 has landed.

There is **no** standalone "add `ref` / `rc` / `weak` / `lref` to `slot_ty`"
task, because those types are never bare slot values. The gate wording in the
non-scalar plan is updated to match.

## Depends on / reuses

- **Drop injection + move analysis** -- `elab_forms.c` (the `(defer (drop! r))`
  injection and its `is_moved` / `is_linear_consumed` guards).
- **Drop emission** -- `rc_strong_decrement` / `drop!` builtin
  (`emit_expr.c`, `emit_module.c`); the rc-elision pairing.
- **N3 struct / ADT support** -- `type_uses_carrier_abi` (Tier A carrier),
  `adt_is_byvalue_product` (Tier C), and their drop glue.
- **The zero-capture cut** -- `has_capture` / `has_capture_case` in
  `emit_cps_ir.c`, which keeps multi-shot capture unreachable.

## Out of scope

- **Lifting the zero-capture cut.** That is the env-capture story; O3 rides it.
- **A per-pointer slot ownership hook.** Findings 1-2 show there is nothing for
  it to hook: owning pointers cross only inside aggregates.
