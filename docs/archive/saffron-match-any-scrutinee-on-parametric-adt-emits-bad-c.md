---
title: "Saffron: `(match s ...)` on an `any` scrutinee emits uncompilable C when the ADT is PARAMETRIC"
category: Reported
description: "In a #lang saffron file, matching an unannotated (any) scrutinee works for a non-parametric ADT and emits uncompilable C for a parametric one -- defgadt and defdata alike. The any-scrutinee narrow declines on an explicit `n_type_params == 0` guard, so no unbox is inserted and the emitter casts the 16-byte tagged aggregate straight to a pointer. Relaxing the guard is not enough: the box holds the ctor's own instantiation, and grounding the target to `any` panics on the mismatch."
---

# A `match` on an `any` scrutinee breaks for a PARAMETRIC ADT

**RESOLVED 2026-09-09** via fix direction 1 (the ctor widen) plus the guard
relaxation it makes sound, with direction 3's diagnostic for the one shape
direction 1 cannot serve. Pinned by `tests/fixtures/saffron-match-parametric-adt`
(both suites run it) and `tests/fixtures/errors/saffron-match-indexed-gadt`.

**What shipped, and what the report got right and wrong:**

- **Right:** the two halves only work together. Half 2 alone was reverted here
  for panicking `cast: any holds a different instantiation`, and that
  reproduced exactly.
- **Right:** `type_adt()` hardcodes `KIND_STAR`, so the arrow kind has to be
  restored with `kind_for_arity` before applying.
- **Wrong, and this was the interesting part:** the `CK_MOVE` question the
  report left open ("whether that is correct for Saffron is its own question")
  was not a design question at all. `CK_UNIQUE` is **0** and `CK_MOVE` is an
  alias for it, so the `any` type argument built with a zeroed `Type` was
  MOVE-typed; the arm binder inherited it and `(* w w)` failed TUR-E0005 on its
  second use. The same `any` as a PARAMETER was fine, because parameters build
  their type through `type_from_kind`. Using that helper is the whole fix --
  nothing about Saffron's ownership semantics needed deciding.
- **Incomplete:** the report's arity table says a GADT "fails identically" to a
  parametric `defdata`, and treats them as one bug. They are two. An
  **indexed** GADT (`(Sq int : (Shape int))`) cannot take the widen at all --
  erasing an index to `any` discards what a GADT exists to carry -- so it
  declines and gets direction 3's diagnostic instead of a C compiler error
  about aggregates. An **unindexed** one (every ctor returning `(Box a)`) also
  declines, silently, because `errors/saffron-gadt-skolem-escape` asserts such
  a match still reaches the skolem-escape check; narrowing it to `(Box any)`
  types the arm binder `tur_tagged_t` over a carrier field. Testing merely for
  `result_type_form` conflated the two and swallowed that fixture's diagnostic.

**Severity was medium.** A *loud* failure -- the emitted C does not compile, so
nothing miscomputes. What it costs is the dialect's headline property: in
Saffron a parameter is unannotated by design, and here it cannot be.

**FILED FIRST AS A GADT BUG, AND THAT WAS WRONG.** The original repro compared
`defgadt Shape [a]` against `defdata Shape (Sq :int)` and concluded the gap was
specific to `defgadt`. It is not: those two differ in *arity* as well, and the
arity is the axis. Measured:

| shape | result |
| --- | --- |
| `(defdata Shape (Sq :int))` -- non-parametric | **works** (36) |
| `(defdata Shape [a] (Sq a))` -- parametric | **fails** |
| `(defgadt Shape [a] (Sq int : (Shape int)))` | **fails**, identically |

A parametric `defdata` fails exactly like the GADT. `defgadt` never mattered.

## Repro

```turmeric
#lang saffron
(defdata Shape [a] (Sq a))
(defn area [s] (match s (Sq w) (* w w)))     ;; `s` is `any`
(defn main [] (println (cast (area (Sq 6)) int)) 0)
```

```
/tmp/tur-build/a-defdata-param_tur.c:7656:13: error: aggregate value used
where an integer was expected
 7656 |   tur_adt_Shape *__scrut = (tur_adt_Shape *)(intptr_t)(s);
```

Annotating the scrutinee (`[s : Shape]`) works, and so does dropping the type
parameter.

## Root cause

`elab_match` has an `any`-scrutinee narrow for Saffron
(`src/compiler/elab_structs.c`) that unboxes to the ADT the arms name. It
declines here, on an explicit guard:

```c
if (only && !mixed && only->n_type_params == 0) {
    Expr *nar = elab_any_unbox_to(e, scrutinee, type_adt(only), ...);
```

So for a parametric ADT no unbox is inserted at all, and the emitter's ordinary
concrete-scrutinee prologue casts the 16-byte `tur_tagged_t` straight to a
pointer.

The branch **is** consulted -- it is not that the GADT path bypasses it. (The
first filing left this open and guessed the opposite way round; the guard is
right there and reading it settles it.)

## ATTEMPTED 2026-09-08 and REVERTED -- relaxing the guard is not enough

The obvious fix -- admit a parametric ADT, narrowing to itself applied to `any`
once per type parameter, exactly as the argument seam grounds `(Vec A)` to
`(Vec any)` (`call_ground_open_app_args_to_any`, elab_call.c) -- was built and
measured. Two things it needs, and then the wall:

- `type_adt()` hardcodes `KIND_STAR`, so a bare `type_app` over it is a
  TUR-E0012 kind mismatch. Restore the arrow kind with
  `kind_for_arity(def->n_type_params)` first, as the ctor result path in
  `elab_call.c` already does.
- With that, **the narrow fires and then panics**:

  ```
  panic: cast: any holds a different instantiation of Shape
  ```

  Because the box holds what the CONSTRUCTOR produced. `(Sq 6)` in a Saffron
  file builds a `(Shape int)`, not a `(Shape any)` -- unlike a container
  *literal*, which the S6 widen makes `(Vec any)`. So the argument seam's
  grounding works there and mismatches here, for a reason specific to how the
  value was built.

  The parametric `defdata` variant also surfaced a second thing on the way: with
  `w` bound at `any`, `(* w w)` is a **TUR-E0005 use-after-move**. An `any` is
  `CK_MOVE`, so a match-arm binder used twice is refused. Whether that is
  correct for Saffron is its own question and is not settled here.

Trading "does not compile" for "panics at runtime" is the wrong direction, so
this was reverted rather than shipped.

## Fix directions

1. **Widen a ctor call's payload to `any` in a Saffron file**, so `(Sq 6)`
   builds a `(Shape any)` and the box matches the narrow's target. This is the
   same answer the S6 literal widen already gives containers, applied to
   user ADTs -- consistent, and it makes "in Saffron, an undetermined type
   argument is `any`" true uniformly instead of true for containers only. It is
   the larger change and the one that fits the dialect's existing story.
2. **Head-match the narrow** -- unbox on the ADT's head, ignoring the
   instantiation. Small, and it is direction 3 of
   `docs/archive/saffron-unannotated-param-container-cast-panics.md`, which was
   considered and rejected there for weakening `cast` for every program rather
   than only Saffron ones. It should not be adopted here without revisiting
   that decision, because the two would then disagree.
3. **Leave the guard and improve the DIAGNOSTIC.** Today the user gets a C
   compiler error about aggregates. A "cannot match an `any` against a
   parametric ADT here -- annotate the parameter" would at least name the
   problem and the workaround. Strictly worse than fixing it, but strictly
   better than what ships now, and it is small.

Direction 1 is the real fix. Direction 3 is worth doing if 1 is not imminent,
because the current failure mode gives the user nothing to act on.

If direction 1 lands, the `CK_MOVE` question above needs an answer in the same
change -- otherwise the parametric `defdata` shape trades a C error for
TUR-E0005.

## Not this bug

`docs/reported/match-arm-binder-in-any-monomorph-typed-as-carrier.md` is a
different defect at a different site: there the SCRUTINEE is fine
(`tur_adt_Box__any __scrut = (b)`) and the ARM BINDER is mistyped as the int64
carrier; here the scrutinee cast is the broken part and the binder is fine.
They were checked against each other, not assumed distinct.

`(Vec any)` and `(Map int any)` are unaffected -- the stdlib collections box
their elements, so their element slot is a pointer, never a by-value tagged
field, and their handles are heap ADTs the narrow never sees.
