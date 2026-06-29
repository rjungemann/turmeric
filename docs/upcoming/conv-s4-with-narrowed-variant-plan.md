---
title: CONV-S4 -- `with` on a Narrowed Multi-Variant ADT
category: Planning
description: Extend `(with v [...])` to the case where `v` is a multi-variant ADT scrutinee that a `match` arm has narrowed to a single record variant. Adds per-arm variant narrowing in the type system so the existing `elab_with_record_adt` path can fire.
---

# CONV-S4 -- `with` on a Narrowed Multi-Variant ADT

## Status

Split out from the parent `struct-adt-convergence-plan.md` (archived
2026-06-28). The two already-landed halves of CONV-S4 (keyword construction
on record variants; `with` on a *single*-variant record ADT) graduated with
the parent plan. What remains is the multi-variant case below.

## Problem

`(with v [f val ...])` lowers to a constructor call through the sole variant
of a single-variant ADT (see `elab_with_record_adt` in
[`src/compiler/elab_structs.c:4834`](../../src/compiler/elab_structs.c)). The
gate is literal:

```c
if (st.kind == TY_ADT && st.as.adt_.def &&
    adt_is_flat_product(st.as.adt_.def) &&
    st.as.adt_.def->n_ctors == 1 && st.as.adt_.def->ctors[0]->is_record) {
    return elab_with_record_adt(...);
}
```

Inside a `match` arm that has already destructured a multi-variant ADT to a
specific record variant, the *intent* is clear -- the programmer has proven
which variant they hold -- but the arm-local binding for the scrutinee still
carries the **full ADT type**, not the variant. The `n_ctors == 1` gate
rejects, and there is no narrowed type to fall through to.

```turmeric
(defadt Shape
  (Circle [radius : float])
  (Rect   [w : float h : float]))

(defn grow [s : Shape] : Shape
  (match s
    (Circle r) (with s [radius {r * 2.0}])   ;; rejected: s : Shape, not Circle
    (Rect w h) (with s [w {w * 2.0}])))      ;; rejected: same reason
```

The arm body knows `s` *is* a `Circle` (or `Rect`) on that branch, but the
type system does not propagate that fact onto `s`.

## Design

The work splits cleanly into a type-system slice and a surface-glue slice.

### S4N-1 -- Per-arm variant narrowing (type system)

Inside a constructor pattern arm `(Ctor ...)`, the scrutinee identifier (when
it is a bare symbol the arm can name) gains a *narrowed view* whose type is
the variant rather than the full ADT.

There are two designs to pick from; pick before implementation:

1. **Implicit narrowing of the scrutinee symbol.** If the scrutinee form of
   `match` is a bare symbol `v`, introduce an arm-local binding shadowing
   `v` with a narrowed type that points at the same value. Cheapest; matches
   how flow-narrowing works elsewhere in the elaborator
   ([`elab_forms.c:1757`](../../src/compiler/elab_forms.c) "TY3: recognise a
   flow-narrowing guard"). Does not help when the scrutinee is a compound
   expression -- but the same is true of `with` on structs today.
2. **Explicit `@`-binding in the pattern.** Add `(Ctor @v ...)` or `(as v
   (Ctor ...))` so the programmer names a narrowed alias. Most flexible;
   costs new surface syntax. Punt unless (1) proves insufficient.

Recommend (1) for v1. A bare-symbol scrutinee in `match` is the overwhelming
common case and matches the cited example.

Implementation sites:

- [`src/compiler/elab_structs.c:3291`](../../src/compiler/elab_structs.c)
  (`elab_match`): when the scrutinee Form is `F_SYM` *and* the pattern is a
  constructor arm on a multi-variant ADT, synthesise an arm-local
  `Binding` for that symbol whose type is a new "narrowed-to-variant"
  view of the ADT.
- The narrowed type is the same `TY_ADT` head with a *single-ctor* `AdtDef`
  view, or a new `TyKind` distinguishing "ADT narrowed to ctor C". The
  cheapest representation is a `TY_ADT` whose `AdtDef *` points at the same
  def but accompanied by a `narrowed_ctor_idx` field (add to `Type::as.adt_`
  in [`types.h`](../../src/compiler/types.h)). `adt_is_flat_product` and
  related predicates ignore the narrowing; only `elab_with` and field
  access consult it.
- Narrowing is **arm-local**: the binding pops off the scope stack at arm
  exit, same as the existing pattern-binding lifetime
  ([`elab_core.c:484`](../../src/compiler/elab_core.c)).

Out of scope: narrowing surviving across an `if`/`when` guard inside an
arm. The narrowed binding is in scope for the whole arm body; existing
flow-narrowing for guards is a separate path.

### S4N-2 -- Teach `elab_with` to accept a narrowed ADT

Once the scrutinee binding inside an arm has a narrowed-to-variant type,
the gate in `elab_with` widens:

```c
if (st.kind == TY_ADT && st.as.adt_.def &&
    adt_is_flat_product(st.as.adt_.def) &&
    ((st.as.adt_.def->n_ctors == 1 && st.as.adt_.def->ctors[0]->is_record) ||
     adt_is_narrowed_to_record_variant(st)))) {
    const CtorDef *ctor = narrowed_or_sole_ctor(st);
    return elab_with_record_adt(e, call, src_form, ovr_form, ctor);
}
```

`elab_with_record_adt` already takes a `const CtorDef *ctor` argument -- it
does not depend on `n_ctors == 1`. Only the gate needs to relax.

`adt_is_flat_product` is retained as a guard because a tagged-multi-variant
ADT *cannot* be reconstructed with a single ctor call -- the lowering
`(let [G v] (Ctor <f0> ...))` produces a `Circle`, which only fits in a
`Shape` slot if `Shape` is a sum that accepts `Circle`. The result is a
*tagged* value of the full ADT type with the ctor's tag word filled in;
existing constructor-call codegen already does exactly this for an arm-body
`(Circle r)` expression. Verify the lowered ctor call typechecks at type
`Shape`, not `Circle`, by following the same path that
`(match s (Circle r) (Circle r))` uses today.

### S4N-3 -- `:copy` requirement

`elab_with_record_adt` already rejects move-only ADTs with TUR-E0296. No
change needed -- the narrowed view inherits `is_copy` from the underlying
`AdtDef`.

### S4N-4 -- Diagnostics

A `with` outside a narrowing context on a multi-variant ADT must explain
*why* it is rejected and *how* to fix it. Today's failure is
"`with: source must be a struct value, got Shape`", which is misleading
once Shape has a record-style variant. New wording (CONV-S6 picks this up
too):

```
TUR-E0300: with on a multi-variant ADT requires a narrowing context.
  'Shape' has 2 variants (Circle, Rect); 'with' can only reconstruct one.
  Wrap the call in a 'match' arm: (match s (Circle r) (with s [radius ...]))
```

Error code is reserved here (next free after TUR-E0299). Confirm the code
is not already taken in [`src/compiler/diag.c`](../../src/compiler/diag.c)
when this lands.

## Fixtures

- `tests/fixtures/conv-with-narrowed-variant/` -- two-variant `Shape`,
  `with` inside both arms, asserts the reconstructed value carries the
  right tag and the overridden field.
- `tests/fixtures/conv-with-narrowed-variant-parametric/` -- the ADT is
  parametric (`(defadt (Box T) (Just [val : T]) Empty)`) and the narrowed
  ctor is `Just`. Confirms type-arg inference still works through the
  narrowed view.
- `tests/fixtures/errors/conv-with-multi-variant-no-narrow-rejects/` --
  bare `(with s [...])` on a `Shape` scrutinee outside any arm. Expects
  TUR-E0300 wording with both variant names listed.
- `tests/fixtures/errors/conv-with-narrowed-non-record-variant-rejects/`
  -- arm narrows to a positional-only variant; `with` rejects with a
  message pointing the user at the missing field names.
- Regenerate `expected.c` snapshots for any fixture whose codegen genuinely
  moves -- expected: none, since the narrowed-view binding affects
  elaboration only.

## Risks

- **Narrowing leaks past the arm.** If the shadowing binding is not popped
  cleanly at arm exit, a later `with s` outside the arm would silently
  succeed with the wrong variant. The fixture
  `conv-with-narrowed-variant-leaks-out` (negative) asserts the post-arm
  type is still `Shape` and `with` is still rejected.
- **Guard-introduced bindings.** A pattern like `(Circle r) when (> r 0.0)
  (with s [...])` must still narrow. Verify by adding the `when` form to
  the primary fixture.
- **GADT arms.** A GADT arm already carries a per-arm skolem env
  ([`elab_call.c`](../../src/compiler/elab_call.c) Phase G2). The narrowing
  must compose with that env: the narrowed type's ctor field types are
  read through the skolem env, not the raw def. Add a GADT fixture to lock
  this in.
- **Parametric ADTs.** The narrowed view must preserve the ADT's type
  arguments. The fixture `conv-with-narrowed-variant-parametric` covers
  this; reuse the same `instantiate` helper struct-field access already
  uses ([`elab_structs.c:442`](../../src/compiler/elab_structs.c) note).

## Order of work

1. **S4N-1a** -- add the `narrowed_ctor_idx` field to `Type::as.adt_`
   (or equivalent representation choice; pick before coding).
2. **S4N-1b** -- in `elab_match`, when the scrutinee is `F_SYM` and the
   arm pattern is a constructor pattern, push a shadowing binding with the
   narrowed type for the arm scope.
3. **S4N-2** -- relax the gate in `elab_with` and pass the narrowed ctor
   to `elab_with_record_adt`.
4. **S4N-3** -- fixtures (positive + negative + parametric).
5. **S4N-4** -- diagnostic wording (TUR-E0300) and its fixture.
6. **S4N-5** -- GADT compatibility check + fixture.

Each step gates on `bash tests/run.sh` (10-minute timeout per the project
rule).

## Open questions

- **Field access on a narrowed scrutinee.** Today `(.field s)` on a
  multi-variant ADT scrutinee inside an arm is rejected because the type is
  the full ADT. Narrowing fixes that for free. Should this plan include
  the field-access fixtures, or are they covered elsewhere? Decide before
  landing S4N-1b -- if covered, just add a sanity fixture; if not, add the
  primary set here.
- **`@`-binding for compound scrutinees.** Punted to v2 unless a real user
  hits it. If/when needed, the binding plumbs through the same narrowed
  type; only the surface parser changes.
- **Cross-arm reuse.** If two arms both want to `with`-update, the
  narrowing is per-arm; no shared infrastructure. Confirmed acceptable.
