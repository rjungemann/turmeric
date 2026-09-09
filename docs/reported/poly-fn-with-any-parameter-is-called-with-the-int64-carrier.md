---
title: "A closure with an `any` parameter passed through `tur_poly_fn_t` is called with the int64 carrier: `fmap` over `(Option any)` emits uncompilable C"
category: Reported
description: "`(fmap (:: (some (:: 41 any)) (Option any)) (fn [x : any] : any x))` fails to compile: `incompatible type for argument 1 of '__fn_1438' -- expected 'tur_tagged_t' but argument is of type 'int64_t'`. The closure's thunk takes the 16-byte box, but the poly-fn shim the constrained HKT method calls it through passes the 8-byte carrier word. Plain Turmeric, static dispatch, no Saffron. It is the one blocker for keying Saffron's dynamic HKT dispatch on the head constructor (D8 question 3): the interpreter already does that and answers 42; the compiled path has nothing sound to call."
---

# A poly-fn with an `any` parameter is called with the int64 carrier

**Severity: medium.** Loud -- uncompilable C, no wrong answer. It blocks any
higher-kinded typeclass method applied to a container of `any` with a closure
that takes the element, which under Saffron is *every* `fmap`/`bind` over a
container built in a Saffron file (S6 and the parametric-ctor widen make those
`(Vec any)` / `(Option any)`).

Found while building saffron-lang-plan D8 question 3 ("key HKT instance rows
on the head constructor"). The dynamic-dispatch side turned out not to be the
hard part; this is.

## Repro -- plain Turmeric

```turmeric
(defn main [] : int
  (let [o (:: (some (:: 41 any)) (Option any))
        r (fmap o (fn [x : any] : any (:: (+ (cast x int) 1) any)))]
    (println (cast (unwrap-or r (:: 0 any)) int))
    0))
```

```
error: incompatible type for argument 1 of '__fn_1438'
note: expected 'tur_tagged_t' but argument is of type 'int64_t'
```

The control -- `(fmap (some 41) (fn [x : int] : int (+ x 1)))` -- prints 42.
So it is specifically the `any` element type.

## Root cause -- measured from the emitted C

`fmap` over `(Option any)` correctly monomorphises to a by-value spec:

```c
static tur_adt_Option__any
__inst_Functor_fmap_Option__spec__tur_adt_Option__any_tur_adt_Option__any_int64_t(
    tur_adt_Option__any container, tur_poly_fn_t g);
```

and `tur_adt_Option__any` stores its payload as a `tur_tagged_t _0` -- a
16-byte box, not a carrier word. The closure the user wrote lowers to

```c
static tur_tagged_t __fn_1438(tur_tagged_t);
```

which is right: its parameter IS an `any`. But the spec receives it as a
`tur_poly_fn_t`, whose calling convention is `int64_t (*fn)(void *, int64_t)`
-- one carrier word in, one out -- and the shim the spec calls through
(`__poly_1440`) hands `__fn_1438` an `int64_t`. A `tur_poly_fn_t` cannot carry
a 16-byte argument, and nothing boxes it on the way through.

So this is an ABI hole at the poly-fn seam: `tur_poly_fn_t` was designed for
carrier-representable arguments, and `any` is the one common type that is not
one. Note `vec-map` over `(Vec any)` with the same closure WORKS (the Saffron
guide's `doubled` example) -- it never goes through `tur_poly_fn_t` at all;
the emitted C for that fixture has zero `__poly_` shims. Only the
typeclass-method path (constrained HKT dispatch, mode-B dictionaries) uses the
poly-fn convention, which is why the hole is where it is.

## Why this blocks D8 question 3, and what is already in place

Saffron's S9 dispatches a typeclass method on an `any` receiver through a
registry keyed on the box tag. For an HKT instance (`Functor [Option]`) the
instance's receiver is the CONSTRUCTOR and a box's tag is the APPLIED type, so
today no row is emitted and the call panics "no instance of Functor for
Option" -- cleanly.

The design for keying it, measured to be feasible:

- The pre-emission scan already records every type widened into an `any`; it
  needs to record the HEAD DEF alongside the id (`emit_abi_note_any_widen`).
- An HKT instance then emits one row PER WIDENED INSTANTIATION whose head
  matches -- `("Functor", id("(type-app Option any)"), table)` -- not one row
  per instance. Head-keying is a row-generation rule, not a different lookup.
- The row's shim must call the **by-value spec for that instantiation**, never
  the carrier base `__inst_Functor_fmap_Option(int64_t, tur_poly_fn_t)`: the
  base reads the element as an int64, and `(Option any)`'s element is a
  16-byte box. The M6/M7 "HKT keeps the uniform carrier ABI" carve-out is sound
  only for carrier-representable elements, which Saffron's are not.
- The shim re-tags the result with the id of `(Option any)`, which it can
  compute statically because it is per-instance.

Every step of that is buildable. The spec it would call is exactly the one that
does not compile here. **The interpreter already does all of this**: its
dispatch keys on the display name, `type_name` of the `Option` constructor is
"Option", and it passes closures natively --

```turmeric
#lang saffron
(defn m [o] (.fmap o (fn [x] (+ x 1))))
;; (unwrap-or (m (some 41)) 0)  =>  42 under `tur interpret`
```

-- so the two back ends currently disagree on whether `.fmap` on an `any` works
at all, and closing that is gated here.

## Fix directions

1. **Box at the poly-fn seam.** When the callee's parameter type is `any`, the
   `__poly_` shim should construct the `tur_tagged_t` from the carrier word --
   which requires the carrier word to BE a pointer to a heap box (as
   `tur_box_*` produces), so the shim can load it -- and the reverse for an
   `any` result. This keeps `tur_poly_fn_t` one-word and localises the change
   to the shim that already exists per closure.
2. **Widen `tur_poly_fn_t`'s convention** to pass arguments as `tur_tagged_t`
   when any parameter is `any`. Larger, and it forks the convention.
3. **Route `any`-element HKT specs through the vec-map path** (direct
   specialisation, no poly-fn). It works there today; the question is why
   mode-B dictionaries need the poly-fn indirection at all when the spec is
   monomorphic.

Direction 1 is the contained one. A fixture wants the repro above plus the
`(Option int)` control, and the Saffron `.fmap`-on-`any` program on both back
ends once the row-generation rule lands.
