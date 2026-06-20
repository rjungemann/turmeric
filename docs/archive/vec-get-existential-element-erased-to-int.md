---
title: vec-get of an existential-element Vec erases the result to int -- open requires a re-ascription
category: Type checking -- carrier-ABI result-kind collapse (Vec[A] where A is existential)
severity: Low-Medium. Not a miscompile and not unsound -- the documented
  call-site ascription recovers the type -- but it is an ergonomics/expressiveness
  hole on the "heterogeneous collection of existentials" path. A `Vec` whose
  element type is a constraint-carrying existential reads back as a bare `int`,
  so `open` (and any existential operation) rejects the element until the full
  `(exists [a] [(C a)] a)` type is restated at every read site. This is the
  friction the turmeric-spices `plot` P3 / Track C investigation hit right after
  the by-value-struct-payload pack defect.
status: resolved
component: compiler/elab (call result-kind) + stdlib/vec
affects: turmeric main 0.21.0
---

# `vec-get` of an existential-element `Vec` erases the result to `int`

## Summary

`(vec-get v i)` is declared `: A`, but for `A = (exists [a] [(C a)] a)` the
call elaborates with result type **`int`** (the int64 carrier), not the
existential. So `(open (vec-get v i) [a x] ...)` fails:

```
error: open: expected existential value (exists [a] T) or ptr<void>, got int
```

Restating the existential type at the read site -- `(:: (vec-get v i)
(exists [a] [(C a)] a))` -- recovers it and the program compiles and runs.
The Vec round-trips the value correctly; only the **static type** of the read
is lost.

## Repro (turmeric main 0.21.0)

```turmeric
(defclass Rdr [a] (rbound [x : a] : int))
(defopaque LinesR :int)
(definstance Rdr [LinesR] (rbound [x : LinesR] (:: x :int)))

(defn main [] : int
  (let [v (vec-new)]
    (vec-push! v (pack (:: 5 :LinesR) (exists [a] [(Rdr a)] a)))
    ;; FAILS: open sees `int`, not the existential
    (open (vec-get v 0) [a x] (rbound x))))
```

Annotating the Vec element type does **not** help -- `vec-get` still yields
`int`:

```turmeric
(let [v (:: (vec-new) (Vec (exists [a] [(Rdr a)] a)))]
  (vec-push! v (pack (:: 5 :LinesR) (exists [a] [(Rdr a)] a)))
  (open (vec-get v 0) [a x] (rbound x)))   ;; still: got int
```

The documented workaround compiles and runs (`=> 5`):

```turmeric
(open (:: (vec-get v 0) (exists [a] [(Rdr a)] a)) [a x] (rbound x))
```

(It also emits a `-Wint-conversion` warning at the `vec-push!` carrier
boundary -- `vec_hypush_ex` takes `int64_t` but receives a `tur_exists_t`
(`void *`) -- a related loose-typing symptom, harmless at runtime since both
are pointer-width.)

## Root cause

`vec-get` (`stdlib/vec.tur:101-111`) has an inline-C body that returns the
raw int64 carrier; the design note at `stdlib/vec.tur:42-48` is explicit that
inline-C bodies are intentionally **not** return-specialized, so the carrier
base always returns int64 and "the call-site `(:: (vec-get v i) :float)`
ascription does the correct union reinterpret." That pattern was designed for
scalar elements (`:float`, sub-word ints).

For an existential element it bites harder because the lost type is not a
scalar reinterpret but a structural type that `open` must see. The collapse
happens in call elaboration: a call's result type is built from the callee's
`result_kind` -- a `TypeKind` (e.g. `type_from_kind(fn_binding->type.as.fn.
result_kind)` in `src/compiler/elab_call.c`). A `TypeKind` cannot carry the
`(exists [a] [(C a)] a)` structure, so the polymorphic `A` is flattened to its
carrier kind `TY_INT`. The substitution `A := (exists ...)` known at the call
is discarded for the inline-C carrier base.

## Fix directions

Lowest-friction, no codegen change:

- **Sticky-ascription ergonomics.** Recognize when a `vec-get` (or any
  carrier-base poly inline-C call) is read at a site whose expected type is an
  existential, and re-ascribe automatically -- the same way result-position
  ascription already re-types sub-word integral carriers in
  `call_wrap_reinterpret` (`elab_call.c:328`). The runtime value is already
  correct; this is purely propagating the statically-known `A` substitution
  onto the call's result type instead of flattening to `result_kind`.

More complete:

- **Carry the substituted full type on the call result.** When the callee's
  declared return is the class/element variable `A` and the call site knows
  `A`'s instantiation (from the `(Vec A)` receiver), set the call's result
  `Type` to the substituted full type rather than `type_from_kind(result_
  kind)`. This generalizes beyond existentials (it would also let a
  `Vec`-of-struct or `Vec`-of-opaque read back with its real type without an
  ascription).

- **Tighten the existential carrier boundary** so `vec-push!`/`vec-get` over
  an existential element do not trip `-Wint-conversion` (accept/return
  `tur_exists_t` through the carrier bridge rather than a bare `int64_t`).

## Workaround

Re-ascribe the read to the existential type at each site:

```turmeric
(open (:: (vec-get v i) (exists [a] [(C a)] a)) [a x] (... x))
```

For the `plot` `AnyRenderer` use case, defining the existential type behind a
`deftype`/macro alias keeps the per-read ascription short, e.g.
`(:: (vec-get v i) AnyRenderer)`. Combined with carrier-representable
(`defopaque ... :int`) renderer handles -- the supported payload shape per
`docs/archive/constrained-exists-pack-struct-payload-bad-cast.md` -- the
heterogeneous-renderer-vec pattern works end to end today, modulo the
ascription boilerplate this report tracks.

## Resolution

Fixed in `elab_call_fn` (`src/compiler/elab_call.c`). When the callee's
declared result is a bare type variable (`vec-get : A`), the call's result
type is instantiated from the receiver's element type via
`call_instantiate_type` -- so a `(Vec (exists [a] [(C a)] a))` receiver
correctly produces the existential as the call result. The bug was the
immediately-following `result_is_concrete_composite` guard, which only
preserved the full type for `TY_APP` / `TY_ADT` / `TY_STRUCT` and collapsed
everything else (including `TY_EXISTS` / `TY_FORALL`) to the int64 carrier.
An existential's carrier is already pointer-width int64, so collapsing it was
pure loss -- it erased the `as.forall_.body` payload that `open` must see.
Adding `TY_EXISTS` and `TY_FORALL` to the guard keeps the instantiated full
type, so `open` accepts `(vec-get v i)` directly.

With the fix:

- A receiver that carries the existential element type -- whether via an
  explicit `(:: (vec-new) (Vec (exists ...)))` annotation (the report's
  example 2, which previously "still: got int") or via the per-read
  `(:: (vec-get v i) (exists ...))` ascription (the documented workaround) --
  now reads back the existential with **no** re-ascription needed at the
  `open`. Regression coverage: `tests/fixtures/vec-get-exists-element/`.

- The fully-bare `(let [v (vec-new)] (vec-push! v ...) (vec-get v 0))` form
  (example 1, no type information anywhere) still needs the element type pinned
  on the receiver. The element type of a fresh `vec-new` is not inferred
  backward from a later `vec-push!` -- `vec-get` reads the int64 carrier for
  any element type -- so a single `(Vec (exists ...))` annotation on the
  binding (or a per-read ascription) is fundamental there, not friction this
  report removes. Backward element inference from `push` into a let-bound Vec
  is a separate, larger inference change.

Not addressed (minor, optional): the `-Wint-conversion` warning at the
existential `vec-push!` carrier boundary (`vec_hypush_ex` takes `int64_t` but
receives a `tur_exists_t` / `void *`). Harmless at runtime (both
pointer-width); tightening the carrier bridge to thread `tur_exists_t` is fix
direction 3 above and left as a follow-up to avoid broad codegen churn.
