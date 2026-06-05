# Instance method that returns an untyped parameter verbatim loses its result type

> **RESOLVED.** Fixed in `elab_method_call` (arrow-identity passthrough): when a
> method's declared return is the arrow class variable (a boxed arity-0 TY_FN
> shell) and its elaborated body is a bare `EX_VAR` reference to one of the
> method's parameters, the dispatch site now recovers that argument's concrete
> arrow signature (forcing the boxed/thunk convention, since a fat parameter is
> returned as a fat box) instead of leaving an uncallable `?` shell. This
> recovers the argument's *real* arity, so it is strictly better than the old
> eta-expansion workaround (which fixed only the unary case).
> `stdlib/arrow-class.tur` now defines `arr` as the bare `(arr [f] f)`.
> Regression coverage lives in `tests/fixtures/arrow-instance-arr-identity/`.
> The original report follows.

**Summary.** A `definstance` method whose body is just one of its (untyped)
parameters resolves to an unknown (`?`) result type at the dispatch site, so
calling the dispatched result fails with `TUR-E0002: function '...' returns ?,
which is not callable` -- even though the *same* value produced through a
sibling method that returns a concrete `(fn ...)` literal is callable.

**Severity.** Ergonomics / expressiveness gap (not a miscompile). It silently
forces an eta-expansion workaround on the most natural definition of identity
methods (`arr`, `id`, `pure`-as-passthrough). No wrong code is generated; the
program simply fails to type-check.

## Minimal repro

```turmeric
(defclass Arrow [a] (arr [f] : a))

;; Body returns the parameter verbatim.
(definstance Arrow [(->)] (arr [f] f))

(defn add1 [x : int] : int (+ x 1))

(defn main [] : int
  (let [a (arr add1)]
    (println (a 5)))     ;; <-- TUR-E0002: 'a' returns ?, not callable
  0)
```

Observed:

```
error [TUR-E0002]: function 'a' returns ?, which is not callable
    (let [a (arr add1)] (println (a 5)))
                                  ^^^^^
```

Expected: `(arr add1)` dispatches to the `(->)` instance and yields the
function `add1` (type `int -> int`), so `(a 5)` evaluates to `6`.

## What works (contrast)

Eta-expanding the body to a concrete closure literal pins the result type and
the dispatched result is callable:

```turmeric
(definstance Arrow [(->)] (arr [f] (fn [x] (f x))))   ;; (a 5) => 6, OK
```

A sibling method that already returns a `(fn ...)` literal (e.g. `>>>`) is also
callable directly:

```turmeric
(definstance Arrow [(->)]
  (>>> [f g] (fn [x] (g (f x)))))
;; (let [h (>>> add1 mul2)] (h 3))  => 8, OK
```

So the trigger is specifically *returning an untyped parameter unchanged*: the
method's declared return type is the class type variable `a`, and when the body
is the bare parameter `f`, the concrete arrow type that `a` should unify with
(here `int -> int`, known from `add1`) is not propagated to the dispatched
call's result type, which stays `?`.

## Root-cause direction

The method's declared return is the class head type variable (`a`). When the
instance body is a parameter reference, the emitter/elaborator does not
back-propagate the argument's concrete type into the method-result type at the
dispatch site (it does when the body is a `(fn ...)` literal, whose type is
known structurally). The fix direction is to unify the dispatched-call result
type with the instance method body's inferred type even when that body is a
bare binding reference -- i.e. treat `(arr [f] f)` the same as `(arr [f] (fn
[x] (f x)))` for result-type inference. Likely lives alongside the
return-type-dispatch machinery in `src/compiler/elab_typeclasses.c` /
`emit_core.c` (the same area that handles nullary arrow return dispatch).

## Workaround in use

`stdlib/arrow-class.tur` defines `arr` eta-expanded (`(fn [x] (f x))`) with an
inline note pointing here. The bare-function layer in `stdlib/arrow.tur` is
unaffected (its `arr` is a free `defn`, not a typeclass method).

## How to validate a fix

Revert the eta-expansion to `(arr [f] f)` and confirm the minimal repro above
compiles and prints `6`, with the full suite still green.
