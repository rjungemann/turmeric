---
status: RESOLVED (2026-07-29) -- the hole now lives in the Type
severity: medium
discovered: 2026-07-29
area: compiler (call-site unification, src/compiler/elab_call.c)
---

# A constrained abstract type constructor only accepts a LAST-param-free head

> **RESOLVED** by fix direction 1 -- putting the hole in the `Type` rather than
> the stopgaps.  `Type.as.app.hole_pos_p1` encodes the free slot (index PLUS
> ONE, so the zero every memset'd Type already carries means "ordinary
> application" and no construction site needed auditing).  Four sites became
> hole-aware:
>
> - `call_collect_type_bindings` (`elab_call.c`) -- the curried match is tried
>   first on a scratch binding set, so existing programs are bit-identical; only
>   on failure does it retry with the element at an earlier slot and bind
>   `m := (Result _ cstr)`.  Last slot keeps priority when both could match.
> - `call_instantiate_type` (`elab_call.c`), `emit_abi_instantiate_type` and
>   `emit_resolve_type` (`emit_module.c` / `emit_core.c`) -- saturating a
>   hole-headed head places the element AT the hole, so `(m b)` becomes
>   `(Result b cstr)` and the spec mangles/materialises as
>   `Result__int__cstr` rather than the transposed `Result__cstr__int`.
> - `type_eq` distinguishes `(Result _ B)` from `(Result B)`; `type_print`
>   spells the hole.
>
> `Monad m => (m int) -> (m int)` now runs at `Result`:
>
>     (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
>       (bind x (fn [v] (ok-int (* v 3)))))
>     (poly-bind (ok-int 4))     ;; => ok 12
>     (poly-bind (bad))          ;; => err "boom"  (ok-biased short-circuit)
>
> Fixture: `hkt-constrained-hole-headed-instance-head` (both arms -- the `err`
> arm proves the ok-biased instance actually ran).  Suite: 2409 passed, 0
> failed, with no fixture churn: the curried-first ordering meant nothing that
> compiled before changed shape.
>
> The analysis below is the pre-fix state, kept as the paper trail.

## Summary

A constrained kind-polymorphic fn abstracts over a unary constructor:

    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int) ...)

The actual argument's constructor may have any arity, but the parameter the
abstraction ranges over must be its **last**. Curried-prefix partial application
(`(Either E)`, leaving `R` free) unifies natively; a head whose free slot is not
last (`(Result _ B)`, leaving the *ok* slot free) does not, and there is no
spelling that makes it work.

This is NOT an arity limit -- binary constructors are fine. `Either` works
today. `Result` fails only because its parameter order is `(Result ok err)`
while its `Functor`/`Monad` instances are ok-biased, so the mapped slot is
first.

This is the only remaining gap in the constrained-HKT area; dispatch, carriers,
and lifted continuations are all resolved (`docs/archive/constrained-hkt-*.md`).

## Repro -- one function, one constructor, only the free slot differs

    (defn probe [^m] [x : (m cstr)] : int 0)      ;; err slot free  (LAST)
    (defn probe [^m] [x : (m int)]  : int 0)      ;; ok  slot free  (FIRST)
    (defn mk [] : (Result int cstr) (ok 1))
    (defn main [] : int (println (probe (mk))) 0)

The first type-checks. The second:

    error [TUR-E0001]: function 'probe' arg 1: expected (type-app tyvar 'm' int),
                       got (type-app (type-app Result int) cstr)

A binary constructor whose LAST param is the abstracted one works end to end,
instance dispatch included -- `Either`, whose `Functor [(Either E)]` head is a
curried prefix:

    (defn poly-fmap [^m] [^Functor m x : (m int)] : (m int)
      (fmap x (fn [v : int] : int (* v 2))))
    (poly-fmap (mk-r 21))        ;; => 42, via Functor [(Either E)]

Pinned by `hkt-constrained-curried-binary-head`.

There is also no ascription escape hatch for the failing direction: `_` is legal
only in a `definstance` head, so `(:: x ((Result _ cstr) int))` is itself a kind
error (TUR-E0012).

## Root cause

`call_collect_type_bindings` (`elab_call.c:399`) unifies structurally over the
curried spine. Expected `(m int)` is `app(tyvar m, int)`; actual
`(Result int cstr)` is `app(app(Result, int), cstr)`. It recurses positionally:

- `fn` vs `fn`: binds `m := (Result int)` -- a curried PREFIX, which fixes the
  ok slot, exactly the one meant to stay free;
- `arg` vs `arg`: `int` vs `cstr` -- mismatch, TUR-E0001.

Curried unification can only ever leave the LAST parameter free. Expressing
"free slot h, others fixed" needs a hole-headed type.

The compiler already has that notion instance-side, but only as a *pair*:
`elab_typeclasses.c:2450` stores `(Result _ B)` as the one-arg application
`app(Result, B)` -- the FIXED arm as the argument -- and records the hole index
separately in `TypeClassInstance.partial_hole_pos` (`typeclass.h:153`). The
`Type` alone is ambiguous: `app(Result, B)` reads as "Result applied to B at
slot 0" under ordinary currying, and as "hole at 0, B at slot 1" only when
`partial_hole_pos` is consulted. Nothing on the unification, substitution, or
result-type path knows about hole positions.

## Fix directions

1. **Put the hole in the `Type`.** A hole index on the TY_APP variant (or a
   dedicated kind) so `m` can bind to `(Result _ cstr)` and substituting into
   `(m b)` yields `(Result b cstr)`. This is the real fix, and it is
   cross-cutting: `type_eq`, `type_print`, substitution, kind checking,
   instance lookup, and the monomorphization name mangling all have to agree.
   Wants its own pass with fixture regeneration.

2. **Consult `partial_hole_pos` at the call site.** When the curried match fails
   and the expected head is a constrained HKT var, look up the constraint
   class's instance for the actual's head ctor, read its hole index `h`, and
   unify the expected element against the actual's arg at `h`. This is enough to
   ACCEPT the argument, and sufficient on its own for a fn whose return does not
   mention `m` -- but a combinator returning `(m b)` still needs (1) to rebuild
   the result type, so on its own it buys little.

3. **Sidestep in the stdlib.** The Route B dict machinery is indifferent to
   parameter order; only unification is not. An err-first alias for `Result`
   -- or `Either`-style curried instance heads on a newtype -- would make the
   ok-biased instances abstractable without any compiler change. Cheapest path
   to "you can write `Monad m =>` over a Result-shaped type", at the cost of a
   second name for the same data.

## Related

- [../archive/constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md)
  -- where this was first noted (as "seam 2", then mis-described as an arity
  limit), alongside the resolved carrier faults.
- [result-monad-bind-typed-boundary-miscompiles.md](result-monad-bind-typed-boundary-miscompiles.md)
  -- `Result`'s troubles on the non-polymorphic `.bind` path; independent
  mechanism, same corner of the stdlib.
- `docs/guides/effects-vs-monads.md` (sharp edges) documents the limitation.
