# M7 stdlib migration: Applicative/Alternative return-directed `pure`/`empty` inference

> **DISPOSITION (2026-06-19): receiver methods migrated; `pure`/`empty` ACCEPTED
> AS GENUINE CARRIER (Phase 4.2).** The RECEIVER-style methods are migrated
> by-value: **`ap`** (`ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)`) and
> **`alt-or`** (`alt-or [x : (f a) y : (f a)] : (f a)`) -- `Option` bodies pure
> Turmeric, combinator instances unchanged, suite 1685/0.
>
> The return-directed **`pure`/`empty`** are kept on the legacy `:int` carrier sig
> as a deliberate Phase 4.2 disposition ("rewrite each helper in pure Turmeric OR
> accept it as genuine carrier"). They are carrier-essential as the stdlib stands:
> the class var appears ONLY in the result, so dispatch needs an expected target,
> and the parser/goal/backtrack APIs erase their container/continuations to the
> `:int`/`:fn` carriers -- an unascribed `(pure x)` in a combinator chain has no
> `(f _)` to dispatch on. Making them by-value is NOT a localized fix: it is a
> parser-library RETYPING pass (combinator args AND continuations typed
> `(Parser A)` / `(fn [A] (Parser B))`) plus generic-dispatch type propagation
> (a generic combinator's `(Parser B)` param currently erases to `int64` at the
> call site). That retyping is the unit of work to flip `pure`/`empty` later; it
> is out of scope for the by-value HKT migration itself and does not block it.
>
> **Phase 5 impact:** the carrier bridge can be TIGHTENED (removed for the
> migrated classes) but the `pure`/`empty` carrier path stays until the retyping
> lands -- consistent with the plan's "tighten/delete" wording.

**Summary.** Migrating the return-directed `Applicative`/`Alternative` methods
`pure` (`(pure [x : a] : (f a))`) and `empty` (`(empty [] : (f a))`) is blocked:
the class variable `f` appears ONLY
in the result, so dispatch needs an expected target type. Real user code calls
`pure`/`empty` without an ascription, relying on the surrounding context -- but
the parser/goal/backtrack combinator APIs type their arguments as the **`:int`
carrier** (Parser/Goal/... are opaque `:int`), so there is no `(f _)` expected
type to propagate to the `(pure x)` argument. The elaborator then reports
`cannot infer type for return-directed method 'pure'`.

`Functor`/`Monad` (receiver-dispatched on `container`/`ma`) do NOT have this
problem -- only the return-directed `pure`/`empty` do.

## Repro

After changing the Applicative class sig to
`(defclass Applicative [^f] (pure [x : a] : (f a)) (ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)))`
and rewriting `Applicative [Option]` by-value, the default suite drops one
fixture: `tests/fixtures/parsec-tutorial/` fails to build with ~7 instances of:

```
error: cannot infer type for return-directed method 'pure'; add a type
  ascription, e.g. (:: (pure ...) T)
```

at call sites like:

```turmeric
(then-parser _close (pure x))           ; then-parser : (fn [int int] int) -- :int carrier
(bind-parser (many ...) (fn [xs] (pure (cons-cell x xs))))
```

`then-parser`/`bind-parser` take `:int` (carrier Parser), so even with
argument-position expected-type propagation the expected type would be `:int`,
not `(Parser _)` -- pure still cannot resolve to the Parser instance.

(The `ap`/`pure` BY-VALUE emit itself works -- `Applicative [Option]` `pure`/`ap`
rewritten in pure Turmeric run correctly with an explicit ascription, e.g.
`(:: (pure 7) (Option int))` -> 7, `(.ap (some add1) (some 41))` -> 42. The block
is purely the unascribed-`pure` INFERENCE in `:int`-erased combinator code.)

## Deeper finding (2026-06-19): an elaborator-only fix is NOT sufficient

An argument-position expected-type push was prototyped (in `elab_call.c`: when a
call arg is a return-directed method call and the callee's param is an applied
HKT type with a concrete head, push that param type as `e->expected_type` so
dispatch resolves the instance). It is necessary but NOT sufficient for the
parsec stdlib, for two reasons found by tracing parsec-tutorial:

1. Some `(pure x)` args land on a param the elaborator sees as the **`:int`
   carrier** (not `(Parser B)`), so there is no head to dispatch on -- those
   combinator params are still `:int`-typed at that site.
2. The harder ones are INSIDE a continuation: `(bind-parser p (fn [xs] (pure
   ...)))`. `bind-parser`'s continuation is typed `f : fn` (the untyped `:fn`
   poly carrier), so the lambda body has NO expected result type at all -- the
   `(pure ...)` there cannot be reached by any caller-param push.

So migrating `pure`/`empty` requires the **parser/goal/backtrack combinator APIs
fully typed** -- not only the direct combinator params as `(Parser A)` (then-parser
already is) but also the **continuations** as `(fn [A] (Parser B))` instead of the
`:fn` carrier (and `bind-parser`/`many`/... results threaded). That is a parser-
library retyping pass, the real unit of work here. The receiver-style `ap`/`alt-or`
migration (which needs none of this) already landed.

## Root cause

`elab_typeclasses.c` return-directed dispatch (~line 3857) uses
`e->expected_type`; with none available and >1 implementing instance it errors
(it never silently picks an instance). The combinator APIs erase their container
to `:int`, so no `(f _)` expected type ever reaches the `pure` call.

## Fix directions

1. **Type the combinator APIs with a real Parser/Goal/... type** (a `defopaque
   Parser :int` newtype, or a parametric `(Parser a)`) instead of bare `:int`, so
   an expected `(Parser _)` propagates to the `pure`/`empty` argument and drives
   return-dispatch. This is the principled fix (it also removes a `:int`
   type-eraser the CLAUDE.md "No Lazy `:int`" rule discourages) but is a
   parsec.tur/logic.tur/backtrack.tur API refactor.
2. **Argument-position expected-type propagation** for return-directed methods
   (thread the callee param's `(f _)` type into the argument elaboration). Needed
   regardless, but inert here until (1) gives the params a real `(f _)` type.
3. **Interim:** keep `Applicative`/`Alternative` on the legacy `:int` carrier sig
   (do NOT migrate) until (1) lands. `Functor` and `Monad` (receiver-dispatched)
   can migrate independently.

## Validation

- `Applicative`/`Alternative` migrated by-value AND `bash tests/run.sh` green
  (no `cannot infer type for return-directed method` errors), with parsec/goal
  combinator code unchanged (no added ascriptions).
