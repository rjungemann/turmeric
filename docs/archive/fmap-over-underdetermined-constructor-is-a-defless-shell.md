# `fmap` over a constructor with an open type parameter yields `(? ?)`

**RESOLVED 2026-09-29** -- see [Resolution](#resolution-2026-09-29): fix
direction 1, with the open parameters marked so they stay the carrier.
`(ok-val (fmap (Ok 41) inc))` prints 42. The same typing gap was also a
**silent wrong answer** this report did not know about: `(ok-val (Ok 7.1))`
printed `4619679907765970534`, the float's bits. Pinned by
`tests/fixtures/ctor-open-param-keeps-payload-type`.

**Severity: low (expressiveness hole; loud, never silent).** Filed
2026-09-28 while making `stdlib/either.tur` generic
([stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md), S3).

## Repro

```turmeric
(defn inc [x : int] : int (+ x 1))
(defn main [] : int
  (println (ok-val (fmap (Ok 41) inc)))
  0)
```

```
error [TUR-E0001]: function 'ok-val' arg 1: expected (Result A B), got (? ?)
```

The same with `(Right 41)` and a generic `from-right` (the shape
`tests/fixtures/sum-either-functor-instance` pins, which passes today only
because `either.tur`'s accessors erase their argument to `:int`).  Annotating
the receiver makes both work:
`(let [r : (Result int int) (Ok 41)] (ok-val (fmap r inc)))` prints 42.

## Root cause

A constructor application of a parametric sum whose parameters are not all
determined by its arguments -- `(Ok 41)` fixes `A` but not `B` -- is typed as
the BARE ADT (`Result`, no applied arguments; a `let` annotation mismatch
reports it as "got adt").  The typeclass dispatch grounds a method's `(f b)`
result against the receiver's TY_APP chain
(hkt-carrier-result-loses-payload-types, `m7_app_replace_slot` in
src/compiler/elab_typeclasses.c), so with no chain there is nothing to ground
and the result stays the def-less `(type-app ? ?)` carrier shell, which no
`(Result A B)` parameter unifies with.

## Fix directions

1. Type such a constructor application as the application with its open
   parameters as fresh type variables -- `(Result int ?B)` -- so the dispatch
   has a chain to ground and a generic consumer unifies (`B` stays open, which
   `ok-val` never needs).  Wide blast radius: every bare-ADT consumer of a
   constructor result sees an application instead.
2. Narrower: let a generic parameter `(T A B)` accept the def-less shell when
   the call site has nothing better, binding nothing.  Cheaper, but it types
   less.

Landing either unblocks the generic `either.tur` recorded in the audit.

## Resolution (2026-09-29)

**What the bare ADT cost.** It lost the determined parameters as well as the
open ones. A generic consumer then bound nothing from its argument, so
`(ok-val (Ok 7.1))`, `(err-val (Err 3.25))` and a user ADT's `(untag (Tag
2.75))` returned the payload's bits as an int, and `(let [r (Ok 7.1)] (match
r (Ok v) ...))` did the same. These were silent wrong answers, not loud
failures.

**The typing (fix direction 1).** `src/compiler/elab_call.c`, the N-ary
constructor result: when a field fixes at least one parameter to a concrete
type and some stay open, the application is built with each open parameter as
the ADT's own type variable. `(Ok 7.1)` is `(Result float B)`, the type the
generic `ok` already gave `(ok 7.1)`. A Saffron file fills the open slot with
`any`, as its nullary constructors already did. A constructor that fixes
nothing concrete stays the bare ADT.

**Keeping the representation.** An application with an open slot is not a
concrete monomorph, so it is the carrier, as the bare ADT was. Each open slot
is a `TY_TYVAR` with the new `tyvar_.open_slot` bit (`types.h`,
`type_has_open_slot`), so the places that must not treat it as a real
variable can tell:

- The constructor's expected-type rescue ignores an open expected type.
  Otherwise a sibling arm's `(Either int R)` filled `(Right d)` out to the
  by-value `(Either int int)`.
- A `match` join (`match_arm_type_compatible`) of two open arms is the bare
  ADT, and a concrete peer wins. An `if` join (`if_branches_unify_via_tyvar`)
  of two open branches is the bare ADT, and an open branch beside a concrete
  one is a mismatch. That is what the bare ADT gave in both cases.
- A `match` binder of an open field (`(Err e)` on `(Result float B)`) is the
  carrier word (`elab_structs.c`), as the whole field was on the bare ADT.
  A variable a signature or instance head quantifies stays abstract; keying
  this on `sig_tyvars` instead turned every instance body's binders into
  ints.
- An unannotated `defn` whose body is an open application records it as its
  return type (`elab_fns.c`). `fn_type_has_named_tyvar` does not count an
  open slot, which is always the carrier.

**The dispatch.** `src/compiler/elab_typeclasses.c`: a receiver with an open
fixed slot grounds `(f b)` to itself with the hole replaced. `(fmap (Ok 41)
inc)` and `(fmap (ok 41) inc)` are `(Result int B)`. Every variable left in
that type came from the receiver, since `b` is ground, so the carrier result
is committed as it is.

**Two float reads.** Both were wrong before this change too, and more of them
are reachable now. `src/compiler/emit_expr.c`:

- On the erased layout, a float binder of a type-variable field reads the
  slot's bits. The switch path's SR2b float leg covered only a binder still
  typed as the variable. A binder already typed `float`, which a
  `(Result float B)` scrutinee gives, took the default `(double)` cast.
- The single-constructor path had no float leg at all.

**A constructor branch beside a by-value branch.** These were broken before
this change, and more reachable after it, so they are fixed here too. Pinned
by `tests/fixtures/match-ctor-arm-beside-byvalue-arm`:

- A literal-pattern `match` (on a bool or an int) with a by-value aggregate
  result initialized its temporary with a scalar `0`, which cc refuses as an
  "invalid initializer". The ADT path next to it already chose `{0}`, and
  now both do (`emit_expr.c`).
- A literal-pattern `match` is typed as its first arm and never joins the
  others. An open first arm (`(Ok 1.25)`) over a later concrete one (`r :
  (Result float int)`) now takes the concrete type, as the ADT path's join
  does. The carrier arm reaches the by-value result through the existing
  box-deref bridge.
- An unannotated `defn` whose body is a concrete application, such as `(::
  (Ok x) (Result float int))`, now records it, as the lambda path has since
  closure-result-monomorphization. Callers saw `(? ?)` before, and a body
  returning the aggregate through the carrier return type reached cc.

**What this does not do.** `(ok 7.1)`'s `B` comes from `ok`'s signature and is
not an open slot, so `(match (ok 7.1) ... (Err e) (println e))` still fails
with "operator lookup failed ... tyvar", as before. An `if` whose branches
are an open constructor and a concrete application is refused with "if
branches have mismatched types", as it was when the constructor was the bare
ADT.
