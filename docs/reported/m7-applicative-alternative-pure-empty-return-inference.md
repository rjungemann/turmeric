# M7 stdlib migration: Applicative/Alternative blocked on return-directed `pure`/`empty` inference

**Summary.** Migrating `Applicative` (and `Alternative`) to the typed by-value
signature is blocked by the **return-directed** methods `pure` (`(pure [x : a] :
(f a))`) and `empty` (`(empty [] : (f a))`): the class variable `f` appears ONLY
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
