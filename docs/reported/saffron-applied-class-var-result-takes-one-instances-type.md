# A dynamically dispatched method whose result is the class variable APPLIED takes one instance's type

**Severity: medium.** A silent wrong answer, compiled only. The interpreter
agrees with the spec. You need a kind-* class whose one-parameter method
returns the class variable inside a constructor (`: (Option a)`), two or
more instances, and a dispatch on an `any` receiver. With a single instance
the answer is right.

Found 2026-09-26 while building saffron-lang-plan S9's "result type from the
class declaration" item. That item landed for a CONCRETE declared result.
This is the part of it that did not.

## Repro

```turmeric
#lang saffron
(defdata Pt (Pt [px : int py : int]))
(defclass Wrap [a] (wrap-self [x] : (Option a)))
(definstance Wrap [int] (wrap-self [x] (some x)))
(definstance Wrap [Pt] (wrap-self [x] (some x)))
(defn w [v] (.wrap-self v))
(defn main []
  (println (is? (w 4) (Option any)))
  (println (is? (w (Pt 1 2)) (Option any)))
  (println (cast (unwrap-or (cast (w 41) (Option any)) (:: 0 any)) int))
  (println (.px (cast (unwrap-or (cast (w (Pt 5 2)) (Option any)) (:: 0 any)) Pt)))
  0)
```

```
$ tur --interpret r.tur        $ tur run r.tur
true                           false
true                           false
41                             panic: cast: any holds a different instantiation of Option
5
```

## Root cause

`EX_DYN_METHOD` needs one result type for every instance, since one slot is
called through one signature. `elab_method_call` (the `Type result_type`
block after the witness loops, src/compiler/elab_typeclasses.c) picks it
like this:

- An HKT class, or a kind-* method that takes extras or returns the BARE
  class variable (`star_witness`), gets an `any`-returning witness per
  instance, so the result is `any`.
- A concrete declared result is read from the class.
- A result that mentions the class variable applied (`(Option a)`) has
  neither path. It falls back to the instance the static resolver happened
  to pick. Here that is `(Option int)`, so the Pt instance's `(Option Pt)`
  comes back typed, and then boxed, as `(Option int)`.

The interpreter has no static result type, so it is not affected.

## What was tried

Treating any result that mentions the class variable as a star witness is
the right shape. `saffron_type_mentions_tyvar` is already in the file for
this. Each instance's witness would re-tag its own result. It was measured
and backed out because the witness it mints does not compile. The Pt
instance's spec comes out as
`__inst_Wrap_wrap_hyself_Pt__spec__tur_adt_Option__Pt_int64_t(int64_t x)`,
with two problems:

- The unannotated class parameter `[x]` is recorded as `int`, not `a`.
- The body's `(some x)` widens `x` to `any` against an `(Option Pt)`
  result, and passes a `tur_tagged_t` to `some__spec_..._tur_adt_Pt`. This
  happens even with the class spelled `[x : a]`.

A plain Saffron `(defn f [x : Pt] : (Option Pt) (some x))` is fine.
`saffron_expected_app_pins` sees its declared result. The instance body
apparently does not have its substituted result as the expected type. With
only ONE instance the backed-out change would have turned a right answer
into a cc error, so it was not worth shipping alone.

## Fix directions

- Give an instance method body its substituted declared result as
  `e->expected_type` (as `elab_defn` does), so the Saffron generic-call
  widen defers to it.
- Then take the star-witness path for any result that mentions the class
  variable, as described above. Pin it with the repro on both back ends.
- Whether the result should be `(Option any)` rather than `(Option int)` /
  `(Option Pt)` is a separate question. The all-`any` instantiation
  principle says a Saffron-built container is `(Option any)`. A TYPED
  class result arguably says otherwise. The interpreter answers `true` to
  `(is? r (Option any))`.
