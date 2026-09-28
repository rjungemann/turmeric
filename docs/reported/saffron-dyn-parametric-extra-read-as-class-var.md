# An `int` extra on a parametric-head instance is cast as the receiver type

**Severity: low-medium.** Compiled only, and a clean panic at a checked cast,
never a wrong answer; `--interpret` answers. It is not limited to
unannotated parameters: an explicitly spelled `n : int` panics the same way.
Split out of [saffron-lang-plan](../archive/saffron-lang-plan.md) S9's
remaining limits (the "unannotated extra on a parametric head" item) when the
plan was archived 2026-09-28.

When a Saffron program dispatches a method on an `any` whose instance has a
parametric head (`Nth [Vec]`), the compiled witness decides per extra
argument whether it is "another value of the receiver's type". An `int` in
both the class and the impl counts as yes, because that is how `Eq`'s
unannotated `(eq? [x y])` records -- so an index argument is cast to
`(Vec any)` and panics.

## Repro

```turmeric
#lang saffron
(defclass Nth [a]
  (nth-of [x n] : any))
(definstance Nth [Vec]
  (nth-of [v n] (vec-get v n)))
(defn pick [x i] (.nth-of x i))
(defn main []
  (println (pick [7.25 1 "hi"] 2))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2):

- `tur --interpret`: prints `hi` (expected).
- `tur run`: `panic at ...: cast: any holds int, not Vec`, exit 134.

Identical results with the class spelled `(nth-of [x n : int] : any)` and
`(nth-of [x : a n : int] : any)`.

## Root cause

`saffron_extra_is_class_var` (`src/compiler/elab_typeclasses.c:6245`): its
last arm, line 6253, returns true when the impl's parameter and the class's
parameter are both `TY_INT`. The caller (the parametric-head arm at line
6433) then casts the extra to the all-`any` instantiation `(Vec any)`. The
arm exists so `(.eq? v w)` on two `(Vec any)` values works, since `Eq [Vec]`
records its unannotated `y` as `int` on both sides; a genuine `int` is
indistinguishable at that point.

The class DOES record the difference for a spelled annotation:
`TypeClassMethod.param_explicit_type` (`src/compiler/typeclass.h:40`, set by
`parse_typeclass_method`) is true for `n : int` and false for a bare `n`. The
helper does not consult it.

## Fix directions

1. Gate the `TY_INT` arm on `!tc->methods[slot].param_explicit_type[j]`, so a
   spelled `: int` is an `int` and only a bare parameter is guessed to be the
   class variable. That fixes the spelled cases above outright.
2. For a bare parameter the guess stays ambiguous (`Eq`'s `y` and this `n`
   record identically). Consult the impl: a parameter used as the receiver's
   type in the body (passed where a `(Vec A)` is expected) versus as an index,
   or pass a bare extra through as `any` and let the impl narrow it -- which
   first needs the method-dispatch path to run the D5 argument seam (the
   kind-`*` witness arm just below notes that it does not).
3. Add a fixture: the repro plus the `n : int` variant, on both back ends.
