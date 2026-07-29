---
status: open
severity: medium
discovered: 2026-07-29
area: compiler (call-site unification, src/compiler/elab_call.c)
---

# A binary type constructor cannot fill a unary constrained type variable

## Summary

A constrained kind-polymorphic fn abstracts over a unary constructor:

    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int) ...)

`Option` fills `m` and works end to end (Route B, 2026-07-29). `Result` cannot:
its head is binary, so filling `m` requires the partially-applied,
error-type-fixed head `(Result _ cstr)` -- exactly the shape the stdlib
instance `definstance Monad [(Result _ B)]` already declares. The instance side
supports it; call-site unification does not, so the last stdlib monad a user
would reach for is shut out of `Monad m => ...` polymorphism entirely.

This is the ONLY remaining gap in the constrained-HKT area -- the dispatch,
carrier, and lifted-continuation bugs are all resolved (see
`docs/archive/constrained-hkt-*.md`).

## Repro

    $ cat > /tmp/r.tur <<'EOF'
    (defn ok-int [n : int] : (Result int cstr) (ok n))
    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (ok-int (* v 3)))))
    (defn main [] : int
      (let [r (poly-bind (ok-int 4))]
        (println (if (ok? r) (ok-val r) -1)))
      0)
    EOF
    $ ./build/tur run /tmp/r.tur
    error [TUR-E0001]: function 'poly-bind' arg 1: expected (type-app tyvar 'm' int),
                       got (type-app (type-app Result int) cstr)

Expected: `12`, dispatching through `Monad [(Result _ B)]`.

There is also no ascription escape hatch: the `_` hole is legal only in
`definstance` heads, so `(:: (ok-int 4) ((Result _ cstr) int))` is itself a
kind error (TUR-E0012). A caller has no spelling at all that makes this call
legal.

## Root cause

The argument-compat check (`elab_call.c`, the TUR-E0001 site around line 4854)
structurally unifies the declared `(m int)` -- one TY_APP with a tyvar head --
against the actual `((Result int) cstr)` -- a two-deep TY_APP spine. Positional
spine unification can only bind `m := (Result int)`, i.e. a curried PREFIX,
which fixes the wrong parameter (the ok-slot, leaving the error type as the
element) and fails on `int` vs `cstr` anyway.

The binding that would make this work is the HOLE-headed partial application
`m := (Result _ cstr)`: fix the error type, leave the ok slot as the element.
The compiler already has the notion instance-side --
`TypeClassInstance.partial_hole_pos` (`typeclass.h:153`) records the `_` slot of
a wildcard head like `(Result _ B)`, and the by-value HKT grounding fixes the
non-hole slots from the receiver. Nothing on the unification/call path knows
about hole-headed types, and `Type` has no representation for one outside an
instance head.

## Fix directions

1. **Represent a hole-headed partial application as a Type** (or an equivalent
   marker on TY_APP), so `m` can bind to `(Result _ cstr)`. Unification of
   `(m a)` against an N-ary application `C t1 .. tN` then has N candidate
   bindings (one per slot the element could occupy); selecting the one for
   which a matching instance head exists (via `partial_hole_pos`) mirrors how
   instance resolution already disambiguates.
2. Route B then needs the instance lookup at the call site
   (`call_dispatched_constraint_class` / the direct-call dict-clone routing in
   `elab_call.c`) to find `Monad [(Result _ B)]` from the bound head -- today it
   walks the spine to the bare ctor and looks up by that, which happens to find
   the right-biased instance for `Result`, so the dict side may need little
   work once unification binds.
3. A narrower stopgap: accept the RIGHTMOST-hole convention only (element = the
   ok slot, all earlier params fixed), which covers `Result`/`Either` and every
   right-biased stdlib instance without general hole search.
4. Surface syntax for hole-headed ascriptions (`((Result _ cstr) int)`) would
   give callers an explicit spelling and make fixtures directly expressible,
   but is not required for inference to work.

## Related

- [../archive/constrained-hkt-byvalue-carriers.md](../archive/constrained-hkt-byvalue-carriers.md)
  -- where this was first noted (as "seam 2"), alongside the resolved carrier
  faults.
- [result-monad-bind-typed-boundary-miscompiles.md](result-monad-bind-typed-boundary-miscompiles.md)
  -- `Result`'s troubles on the non-polymorphic `.bind` path; independent
  mechanism, same user-visible corner of the stdlib.
- `docs/guides/effects-vs-monads.md` (sharp edges) documents the limitation.
