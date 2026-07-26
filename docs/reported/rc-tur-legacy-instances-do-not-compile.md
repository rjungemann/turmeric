---
status: open
severity: medium
discovered: 2026-07-26
area: stdlib (stdlib/rc.tur typeclass instances)
---

# `(load "stdlib/rc.tur")` does not compile

## Summary

`stdlib/rc.tur` cannot be loaded into a compiled program. Its three legacy
typeclass instances -- `__functor_rc_fmap`, `Functor [rc]`, and `Foldable [rc]`
-- call four runtime functions that **exist nowhere in the tree**:

    tur_rc_of   tur_rc_clone   tur_rc_ptr   tur_rc_drop

`grep -rn 'tur_rc_of\|tur_rc_clone\|tur_rc_ptr\|tur_rc_drop' src/` returns
nothing. They are not in `src/runtime/rc.h`, not in the emitted preamble, not
anywhere. The real API is `rc_cb_alloc` / `rc_strong_increment` /
`rc_get_value` / `rc_strong_decrement`.

`Foldable [rc]` has a second, independent break: its bodies cast the `fn`
parameter with `(int64_t(*)(int64_t, int64_t))(intptr_t)fn`, but under the
by-value HKT path `fn` lowers to a `tur_poly_fn_t` **struct**, so the cast is
`error: aggregate value used where an integer was expected`. `Functor [rc]` was
migrated to `fn.fn(fn.env, arg)` at some point; `Foldable [rc]` was not. The
`;; M7 migration NOTE` comment at `stdlib/rc.tur:98-102` claims the raw-pointer
cast is still valid ("verified: suite byte-identical") -- it is not, and the
suite could not have caught it, because no fixture loads `rc.tur`.

## Severity

Medium. Nothing in-tree loads `rc.tur` today (the `rc/of` / `rc/clone` /
`rc/drop` / `rc->ptr` surface people actually use is built into the compiler as
intrinsics, not defined by this module), so the breakage is latent. It becomes
user-visible the moment anyone tries to use the `Functor` / `Foldable` / `Clone`
instances the module advertises, or loads it for any other reason.

It is also why the WR1 `weak<T>` API from
`docs/upcoming/v1/stdlib-weak-ref-audit-plan.md` landed in a new
`stdlib/weak.tur` rather than in `rc.tur` as the plan text says: an API in a
module that cannot be loaded is not an API.

## Repro

    $ printf '(load "stdlib/rc.tur")\n\n(defn main [] : int 0)\n' > /tmp/r.tur
    $ ./build/tur build /tmp/r.tur -o /tmp/r
    ...
    error: aggregate value used where an integer was expected
      return ((int64_t(*)(int64_t, int64_t))(intptr_t)fn)(init, value);
    warning: implicit declaration of function 'tur_rc_clone'
    warning: implicit declaration of function 'tur_rc_of'
    warning: implicit declaration of function 'tur_rc_drop'
    tur: cc invocation failed (status 256)

`tur check stdlib/rc.tur` exits 0 -- the module type-checks fine. Only the C
stage fails, which is why this has stayed invisible.

Verified pre-existing: the same failure reproduces on a clean tree at
`f4493704`.

## Root cause

`stdlib/rc.tur:31-55` (`__functor_rc_fmap`), `:65-76` (`Functor [rc]`), and
`:107-123` (`Foldable [rc]`) are carrier-era inline-C written against an API
that was either renamed or never existed. Nothing links them, so nothing
noticed.

## Fix directions

1. **Rewrite the bodies against the real runtime API.** Mechanical for the two
   Functor bodies (`tur_rc_ptr` -> `rc_get_value`, `tur_rc_clone` ->
   `rc_strong_increment`, `tur_rc_drop` -> `rc_strong_decrement`, `tur_rc_of` ->
   `rc_cb_alloc` + assign `cb->value`). Note the clone/drop dance in both
   Functor bodies is pointless -- they clone only to read the value and drop it
   again.

2. **`Foldable [rc]` needs a decision, not a rename.** The 2-arg fold function
   arrives as a 1-arg `tur_poly_fn_t`, so calling it from inline C means
   applying `fn.fn(fn.env, a)` and then invoking the returned fat closure --
   ABI-sensitive and easy to get wrong. A pure-Turmeric body would be better,
   but it hits a separate gap: an `rc<A>` extractor cannot be called on the
   instance's `ta`, because the HKT container type `(t a)` instantiates to a
   `TY_APP` (`(type-app rc<?> tyvar 'a')`) which does not unify with the
   `TY_RC` a `[^borrow r : rc<A>]` parameter expects:

       error [TUR-E0001]: function '__rc-value' arg 1:
         expected rc<?>, got (type-app rc<?> tyvar 'a')

   Fixing that unification is the real prerequisite.

3. **Or delete the instances.** They have never worked, so nothing can depend on
   them. KB-027 notes `rc` is the home type for the `Functor`/`Clone` dispatch,
   so check what `tur check` of the wider stdlib needs before removing.

Once `rc.tur` compiles, `stdlib/weak.tur` could be folded back into it (or kept
separate deliberately -- it is a clean opt-in module either way).

## Related

- `docs/upcoming/v1/stdlib-weak-ref-audit-plan.md` (WR1)
- `docs/guides/ownership-guide.md`
