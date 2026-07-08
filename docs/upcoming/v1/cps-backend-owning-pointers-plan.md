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

### O1 -- drop-node translation (the CPS-specific remainder)

- Translate `EX_DEFER` (whose body is a `drop!` / `rc/drop` builtin) and
  `EX_RC_DROP` in `cps_ir.c` into a CPS-IR drop node (a `CT_LETPRIM`-shaped
  eff?-free statement, or a dedicated `CT_DROP`), threaded so the drop runs after
  the guarded value is last used and before the block's value is delivered to the
  continuation.
- Emit it in `emit_cps_ir.c` as the corresponding `rc_strong_decrement` /
  `drop!` call.
- Keep the move elision the front end already computed: only the non-elided drop
  nodes are present in the tree, so faithfully translating "the drops that are
  there" is correct by construction.
- Guard: retain the zero-capture cut; a drop node inside a *captured* /
  multi-shot continuation stays on the fallback until O3.
- Round-trip fixture: a colored function that binds an `rc` locally, reads it,
  drops it, and delivers a scalar through a `shift` -- direct-vs-CPS equal and
  LeakSanitizer-clean.

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

1. O1 (drop-node translation) has landed, so a colored function with a local
   owning value CPS-emits with its drop run exactly once (LeakSanitizer-clean);
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
