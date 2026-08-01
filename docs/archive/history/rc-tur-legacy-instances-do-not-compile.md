---
status: resolved
severity: medium
discovered: 2026-07-26
resolved: 2026-07-26
area: stdlib (stdlib/rc.tur typeclass instances)
---

# `(load "stdlib/rc.tur")` does not compile

## Resolution (2026-07-26)

Fixed via fix directions 1 and 2, plus 3 for the one body that was beyond
repair. `(load "stdlib/rc.tur")` compiles and the instances compute correct
results.

- **`Functor [rc]` / `fmap`** -- rewritten against the real API: `rc_get_value`
  to read the payload, then `rc_cb_alloc(0, 3 /* TY_INT */, NULL)` plus a
  malloc'd slot to box the result, mirroring exactly what `rc/of` emits. The
  clone/drop pair around the read is gone: it cloned the block only to read one
  field and drop it again -- a no-op with two refcount writes -- and `container`
  is borrowed for the call, so the block cannot go away underneath it.

- **`Foldable [rc]` / `foldl` + `foldr`** -- the `tur_poly_fn_t` misuse is fixed
  by calling the 2-arg convention the compiler itself emits for a typed 2-arg
  `fn` parameter:

      ((int64_t (*)(void *, int64_t, int64_t))fn.fn)(fn.env, init, value)

  Cast the `fn` slot to a signature taking `env` first, then the arguments. It
  is **not** curried, which is what fix direction 2 above guessed wrong.
  Determined empirically from the codegen for
  `(defn apply2 [f : (fn [int int] int) a : int b : int] : int (f a b))`, which
  emits precisely this shape. So no compiler change was needed here, and the
  TY_APP-vs-TY_RC unification gap turned out not to be a prerequisite -- it
  remains open as a separate cleanup, filed as
  `docs/reported/hkt-fmap-result-is-not-droppable.md`, which would let these
  bodies drop their inline-C entirely.

- **`__functor_rc_fmap`** -- deleted (fix direction 3). Nothing referenced it,
  and its untyped params made `container` an `:int`, so it could not be passed an
  actual `rc<T>` at all (`expected int, got rc<int>`) -- the CLAUDE.md
  :int-stand-in defect in its purest form. `Functor [rc]` supersedes it.

- **`Clone [rc]`** -- untouched. It already used the real `rc_strong_increment`
  and was never part of this defect.

**The root cause was the missing fixture**, so that is the durable part of the
fix: `tests/fixtures/rc-tur-typeclass-instances` loads the module and asserts
values rather than just "it compiles". It folds with the non-commutative `sub`
in both directions, so a swapped or garbage argument from a wrong poly-fn
convention shows up in the output instead of silently passing.

`stdlib/weak.tur` (WR1) stays a separate module rather than folding back in --
now by choice rather than necessity. Reaching for a weak reference should not
drag in rc.tur's Functor/Foldable/Clone instances and the typeclass surface
behind them; same shape as `rcchain.tur`.

Two adjacent defects found while fixing this and filed separately:
`docs/reported/hkt-fmap-result-is-not-droppable.md` (an `fmap` over an `rc`
returns `(type-app ? ?)`, so its result cannot be `rc/drop`ped) and
`docs/reported/c-keyword-function-names-not-mangled.md` (a Turmeric function
named `double` emits an unmangled C identifier).

The original report follows for the record.

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
`docs/archive/stdlib-weak-ref-audit-plan.md` landed in a new
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

- `docs/archive/stdlib-weak-ref-audit-plan.md` (WR1)
- `docs/guides/ownership-guide.md`
