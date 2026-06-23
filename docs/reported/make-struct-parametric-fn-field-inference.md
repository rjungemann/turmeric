---
status: open
severity: medium
discovered: 2026-06-23
discovered-by: completing parametric-defstruct-fn-field-gaps (residual)
---

# make-struct can't infer a parametric struct's type args from fn-typed fields

## Summary

`make-struct` on a *parametric* struct cannot infer the struct's type
parameters when a parameter appears only inside a fn-typed field. Instead of
unifying the tyvar against the supplied function value's type, it defaults the
tyvar (to `int`, or `ptr<void>`), then rejects the argument.

This is the residual left after `parametric-defstruct-fn-field-gaps.md` (Gaps
1, 2, 4 fixed; Gap 3 was a name collision). With those fixes the parametric
`Lens`/`Endo` structs now *parse, scope, and kind-check*; only construction-site
type inference remains.

## Repro A -- tyvar appears only inside a fn field

```turmeric
(defstruct Endo [A] (run (fn [A] A)))
(defn inc [x : int] : int (+ x 1))
(defn main [] : int
  (let [e (make-struct Endo inc)]   ;; A should infer to int from inc
    0))
```

```
error: make-struct 'Endo': could not infer type parameter 'A' from field
values (it appears in no field; add a type ascription, e.g.
(:: (make-struct Endo ...) (Endo ...)))
```

`A` *does* appear in the field -- inside `(fn [A] A)` -- but the inference
walk does not look through the fn type to unify `A` against `inc`'s
`(fn [int] int)`.

## Repro B -- multi-param Lens defaults to int

```turmeric
(defstruct Lens [S A]
  (get (fn [S] A))
  (put (fn [S A] S)))
(defstruct Person :copy [name : cstr age : int])
(defn name-get [p : Person] : cstr (.name p))
(defn name-put [p : Person n : cstr] : Person (make-struct Person n (.age p)))
(defn main [] : int
  (let [l (make-struct Lens name-get name-put)] 0))
```

```
error: make-struct 'Lens': field 'get' expects (fn [int] : int),
got (fn [<struct>] : cstr)
```

`S`/`A` were defaulted to `int` rather than unified from `name-get`/`name-put`.

## Workaround

Ascribe the constructed value, supplying the type args explicitly:

```turmeric
(:: (make-struct Endo inc) (Endo int))
```

The monomorphic form (no struct type params; fn fields mention concrete
struct/cstr types) works end-to-end -- see
`tests/fixtures/defstruct-fn-field-struct-cstr`.

## Suspected location / fix direction

The make-struct type-arg inference (the walk that maps each field's declared
type back onto the supplied value to solve the struct's `[type-params]`) needs
to descend into a field whose type is `TY_FN` and unify the declared
arg/result types (including nested tyvars) against the argument value's fn
type, the same way it already unifies a plain `(val A)` field. This is
parametric-construction inference and likely wants to ride along with the
broader monomorphization work -- see `[[project_monomorphization_north_star]]`.
