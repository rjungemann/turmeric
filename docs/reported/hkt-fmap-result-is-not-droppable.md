---
status: open
severity: low
discovered: 2026-07-26
area: compiler (HKT result typing, elab_typeclasses.c)
---

# `(fmap r f)` over an `rc<T>` returns an untyped `(type-app ? ?)`, so it cannot be dropped

## Summary

`Functor [rc]`'s `fmap` allocates a fresh `rc` for its result -- that is what
`fmap :: (a -> b) -> rc a -> rc b` means. But the call's result type comes back
as the abstract `(type-app ? ?)` rather than `rc<b>`, so there is no way to
release it:

    error: rc/drop requires rc<T> or a constrained existential,
           got (type-app ? ?)

The value is usable (it can be folded, passed on), just not droppable. Every
`fmap` over an `rc` therefore leaks one control block plus one payload slot.

## Repro

    (load "stdlib/rc.tur")

    (defn dbl [x : int] : int (* x 2))
    (defn add [a : int b : int] : int (+ a b))

    (defn main [] : int
      (let [r (rc/of 21)]
        (let [m (fmap r dbl)]
          (println (:: (foldl m 0 add) int))   ; 42 -- the value is fine
          (rc/drop m)))                        ; error: got (type-app ? ?)
      0)

Note the fold works, so this is specifically about the result *type*, not the
value. `(:: (foldl m 0 add) int)` needs its own ascription for the same
underlying reason.

## Severity

Low. It was previously unreachable -- `rc.tur` did not compile at all until
2026-07-26 (`docs/archive/rc-tur-legacy-instances-do-not-compile.md`), so no
program could have hit this. Nothing in stdlib uses `fmap` over an `rc`, and
`rc.tur` is opt-in. Filed so the hole is on the record now that the module can
actually be loaded.

## Root cause (suspected, not confirmed)

The class method's declared result is `(f b)` over the class's type constructor
variable. Instantiating the instance head `[rc]` should ground that to `rc<b>`,
but the result stays a `TY_APP` with both positions unresolved. This is adjacent
to the gap noted in `docs/archive/rc-tur-legacy-instances-do-not-compile.md` fix
direction 2: on the *parameter* side, `(t a)` instantiates to
`(type-app rc<?> tyvar 'a')` and does not unify with a `rc<A>` parameter either.
Both look like the same missing normalization of a `TY_APP` whose head is a
built-in pointer-family type constructor down to the concrete `TY_RC` form.

## Fix directions

1. Normalize `TY_APP(rc, X)` to `type_rc(kind of X)` (and the `weak`/`ref`
   equivalents) when instantiating an instance method's parameter and result
   types. Fixing both sides together is probably one change.
2. With that in place, `Foldable [rc]`'s bodies could drop their inline-C
   entirely and be written in pure Turmeric -- see the fix-direction-2 note in
   the archived report, which is blocked on exactly this.
3. Pin with a fixture that `rc/drop`s an `fmap` result and asserts the strong
   count of the original is untouched.
