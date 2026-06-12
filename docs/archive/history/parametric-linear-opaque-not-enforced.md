---
title: `defopaque [T] :int :linear` does not enforce single-use under `-Xsubstructural`
category: Archived (fixed 2026-06-11)
severity: Silent capability-system hole -- linear discipline is dropped for any opaque carrying a phantom type parameter
discovered: 2026-06-11, while landing Phase I1 of the ECS write-cap plan (`ecs-defsystem-write-caps-not-enforced.md`)
location: `src/compiler/types.c` (`type_app`, `substitute_struct_app_type`, `substitute_adt_app_type`) and `src/compiler/elab_types.c` (`type_expr_from_form`) -- four TY_APP construction sites hardcoded `copy_kind = CK_COPY`, dropping the head's `:linear`/`:affine` qualifier.
resolution: Fixed 2026-06-11 by adding `propagate_app_discipline(Type *app, const Type *fn)` in `src/compiler/types.c` and invoking it at all four TY_APP construction sites. The helper walks the `fn` spine to its head and, when the head is a `:linear` or `:affine` opaque/struct, lifts the discipline (CK_LINEAR / CK_UNIQUE + SK_AFFINE) onto the application node so the downstream binding-marking checks at `elab_fns.c:1369` and `elab_forms.c:636` fire correctly. Regression-tested by `tests/fixtures/errors/parametric-linear-double-use/`.
---

# `defopaque [T] :int :linear` does not enforce single-use under `-Xsubstructural`

## Summary

A non-parametric `:linear` opaque triggers TUR-E0101 on double-use as
expected. The *parametric* form -- a `defopaque` with a type-parameter
list followed by the same `:linear` qualifier -- compiles a double-use
program with **no diagnostic at all**, even under `-Xsubstructural`
(and `-Xsubstructural -Xlinear`).

Net effect: any capability or handle that uses a phantom type parameter
to nominally distinguish instances (`WriteCap<Pos>` vs. `WriteCap<Vel>`,
`SChan<P>`, `Parser<A>`, ...) silently loses its `:linear` discipline.

## Repro

Non-parametric (works as expected):

```turmeric
(defopaque Token :int :linear)

(defn make-token [n : int] : Token
  ```c
  return (int64_t)n;
  ```)

(defn use-token [^linear t : Token] : nil
  ```c
  (void)t;
  ```)

(defn main [] : int
  (let [t (make-token 0)]
    (use-token t)
    (use-token t))   ;; TUR-E0101: linear value 't' used after being consumed
  0)
```

`tur check -Xsubstructural` correctly reports:

```
error [TUR-E0101]: linear value 't' used after being consumed
```

Parametric (silently accepts double-use):

```turmeric
(defopaque Token [T] :int :linear)

(defn make-token [T]
  [n : int]
  : (Token T)
  ```c
  return (int64_t)n;
  ```)

(defn use-token [T]
  [^linear t : (Token T)]
  : nil
  ```c
  (void)t;
  ```)

(defstruct Pos [x : int])

(defn main [] : int
  (let [t (:: (make-token 0) (Token Pos))]
    (use-token t)
    (use-token t))   ;; expected TUR-E0101 -- but compiles cleanly!
  0)
```

`tur check -Xsubstructural` returns exit 0 with no diagnostic.

Both forms are otherwise identical: same carrier type (`:int`), same
`:linear` keyword, same `^linear` parameter annotation on the consumer.
The only difference is the `[T]` parameter list on the opaque head.

Reproduced with `./build/tur --version` reporting `turmeric 0.19.1`.

## Observed vs. expected

Observed: parametric `defopaque [T] :int :linear` is treated as a
non-linear nominal type. `^linear` params on functions taking it do
not cause TUR-E0100/E0101 to fire.

Expected: the parametric form is identical to the non-parametric one
modulo the phantom. The `:linear` qualifier should propagate through
the head and gate all uses, exactly as it does for the non-parametric
case. A double-use must report TUR-E0101.

## Why this is load-bearing

Several stdlib types already declare parametric `:linear` opaques and
appear to rely on the discipline:

- `stdlib/schan.tur`: `(defopaque SChan [P] :ptr<void> :linear)` -- the
  whole protocol-soundness story rests on single-use.
- `stdlib/future.tur`: `(defopaque Promise :ptr<void> :linear)` is
  non-parametric and fine; if Promise gets a phantom in the future, it
  silently loses linearity.

The pending ECS `WriteCap<T>` capability system
(`ecs-defsystem-write-caps-not-enforced.md`, Phases I1-I4) is the
proximal motivator: a phantom-parameterized `WriteCap<T>` is exactly
how we'd nominally distinguish per-component write capabilities, and
exactly what the parametric-linear hole turns into a no-op.

If any program in the wild compiles cleanly today against an `SChan<P>`
protocol with an internal double-use, the soundness claim of that API
is currently a runtime claim, not a type-system claim.

## Suspected root cause

A reasonable guess (untested -- pointer for the fixer, not a finding):
the substructural checker probably reads `is_linear` off the opaque's
*head* descriptor when the type appears non-parametrically, but when the
type appears as an application `(Token Pos)` it goes through a
type-application path that builds a fresh type node and forgets (or
never copies over) the substructural qualifier from the head.

Files worth probing first:

- The opaque-head registration in the type-decl path.
- The type-application / instantiation path in the type checker.
- The substructural-qualifier propagation from a type variable's
  underlying head to an applied instance.

## Proposed fix direction

Propagate the `:linear` (and `:affine`, `:relevant`) qualifier from the
opaque's head to every instantiation of an application. The qualifier
is an attribute of the head, not the type parameters, so it survives
substitution: `(Token T)` for any `T` should be linear iff `Token` is
linear. Apply the same fix to `:affine` and `:relevant` if they suffer
the same hole.

## Validation a fix is in place

- Adding `[T]` to a known-good `defopaque ... :linear` must not change
  its discipline -- double-use must still fail with TUR-E0101.
- The parametric repro above (`Token [T]`) must report TUR-E0101 on
  line 16 of the second snippet.
- `stdlib/schan.tur` -- write a fixture that double-consumes an
  `SChan<P>` and assert it fails the substructural checker.

## Workarounds

For callers that need a parametric linear opaque today:

- **Per-instance monomorphic opaques.** Replace `WriteCap<Pos>` with a
  `defopaque PosWriteCap :int :linear` minted by a macro per component.
  Linear discipline survives because the opaque is non-parametric.
  The macro recovers the nominal-distinction property the phantom
  would have given.
- Avoid `:linear` on the head; require every consumer to take its
  argument `^linear`. Doesn't help here -- the experiment in the second
  repro snippet already does this and the discipline still doesn't
  fire.

## References

- `docs/reported/ecs-defsystem-write-caps-not-enforced.md` -- the ECS
  write-cap plan whose Phase I1 surfaced this gap.
- `docs/guides/substructural-types-guide.md` -- the substructural
  contract this report claims is broken for parametric opaques.
- `stdlib/schan.tur`, `stdlib/chan.tur`, `stdlib/future.tur` --
  current stdlib `:linear` opaque consumers.
