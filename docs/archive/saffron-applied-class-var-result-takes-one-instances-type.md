# A dynamically dispatched method whose result is the class variable APPLIED takes one instance's type

**RESOLVED 2026-09-26.** The measurement below understated it: the compiled
result was not tagged as the *first* instance's type but as one unresolved
`(Option a)`, so even `(is? (w 4) (Option int))` was false. The cause sat a
layer below dispatch. Pass 1 of `definstance` substituted the class variable
into a result that IS the variable (`: a`) or an HKT head (`(f b)`), but not
into one that merely contains it: `Wrap [Pt]`'s impl kept returning the open
`(Option a)`, which lowers to the carrier. Four changes, each needed:

- **Substitute through the application.** `elab_subst_class_tyvars` turns
  `(Option a)` into `(Option Pt)`. Three guards apply. The class must be
  kind-*. The method must be receiver-dispatched: a return-directed one such
  as `(dec [seed : int] : (Result a cstr))` rides the uniform carrier every
  such dispatch reads. The instance types must be ground (a primitive or a
  non-parametric ADT): `Dec [Option] [(Dec A)]` would otherwise substitute
  the bare constructor. The first unguarded attempt broke ten decode and
  return-dispatch fixtures, which is how each guard was found.
- **The body sees its declared result.** A ground applied result goes onto
  `e->expected_type` for the instance body, as `elab_defn` does for a defn.
  In a Saffron file `(some x)` then builds `(Option Pt)` instead of widening
  `x` to `any`.
- **The per-instance witness.** `star_witness` now covers any result that
  mentions the class variable, not only a bare one. Each instance's witness
  returns `any` and boxes its own instantiation.
- **The dict slot agrees with the impl.** With the result ground, a
  carrier-ABI result (`(Vec float)`) made the dict typedef and the by-value
  wrapper spell `tur_adt_Vec__float *` against an `int64_t` impl.
  `dict_slot_ret_c_name` now uses the impl signature's rule.

Two more came with it:

- **Saffron data literals honour a pinning expectation.** Found through the
  `(Vec a)` case, and a silent wrong answer in plain Saffron:
  `(defn f [x : float] : (Vec float) [x x])` built a `(Vec any)` typed
  `(Vec float)`, and `(vec-get (f 7.1) 1)` printed `4.6e-310`, even under an
  explicit `(:: [..] (Vec float))`. `[..]`, `#set{..}` and `#map{..}` now skip
  the element widen when the expectation pins the instantiation
  (`dl_saffron_expected_pins`), as a generic constructor call already did.
- **A mismatched applied instance result is a type error**, not a cc error.
  An instance body that builds a different instantiation (`(Option int)`
  under `Wrap [Pt]`'s `(Option Pt)`) now gets a diagnostic naming both
  types.
- **This report's own repro is now an error.** The class leaves `[x]`
  unannotated, so by the typed rule `a` appears only in the result and
  `wrap-self` is return-directed: every instance's `x` is `int`. No witness
  keyed on the receiver can call those impls. Typed code already refuses
  the static form of the call (`(.wrap-self 41)`). So a dynamic dispatch of
  such a method is now TUR-E0020, with a hint to spell `[x : a]`. Before, it
  silently mis-tagged.

The interpreter's dynamic arm also boxes a collection result under the
impl's declared type when the node is `any`. Otherwise `type-of` on the
`(Vec float)` read `int`.

Answers now agree on both back ends when the class spells the receiver
`[x : a]`. The report's original lines 1-3 used `(is? r (Option any))`.
That differs between the back ends for any typed `(Option int)` behind an
`any`, a plain defn's included: compiled `is?` compares the instantiation,
the interpreter treats `any` as a wildcard. That is not this report's.

Pinned by:
- `saffron-dyn-applied-class-var-result` (Option and Vec results, two
  instances and one)
- `typeclass-applied-class-var-result` (typed, static)
- `saffron-literal-under-typed-return`
- `errors/instance-applied-result-mismatch`
- `errors/saffron-dyn-return-directed-applied-result`

The original report follows.

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
