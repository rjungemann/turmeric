---
status: open
severity: medium
discovered: 2026-07-29
area: stdlib (stdlib/macros.tur `for`, return-position dispatch)
---

# The `for` comprehension is unusable against the auto-loaded stdlib

## Summary

`for` desugars its body to `.pure`, which is a **return-position** method: it
has no receiver, so it dispatches on the expected type. Inside the desugaring
the `.pure` sits in a `fn` body with no expected type, so it has nothing to
resolve against. With two `Applicative` instances auto-loaded (`Schema` and
`Option`), every use of `for` over a monad fails:

    /home/user/turmeric/stdlib/macros.tur:79:49: error []: ambiguous method
    dispatch: '.pure' matches 2 instances (Applicative[Schema], Applicative[Option])
    -- receiver type is erased (int64_t).

This is not a user error that annotation can fix -- the ambiguous call site is
*inside the macro*, so there is nowhere for the caller to put a hint. `for` is
effectively dead surface as shipped.

`do-m` is unaffected: it only desugars to `.bind`, which is receiver-directed.

## Repro

    $ cat > /tmp/f.tur <<'EOF'
    (defn half [x : int] : (Option int)
      (if (= x (* 2 (/ x 2))) (some (/ x 2)) (none)))
    (defn sums [] : (Option int)
      (for [x (half 20) y (half x)] (+ x y)))
    (defn main [] : int (println (unwrap-or (sums) -1)) 0)
    EOF
    $ ./build/tur run /tmp/f.tur
    /home/user/turmeric/stdlib/macros.tur:79:49: error []: ambiguous method dispatch: '.pure' ...

Expected: `15`. Wrapping in a `(defn ... : (Option int))` does not help -- the
`.pure` is nested inside the `fn` the macro builds, so the outer return type
never reaches it. The `do-m` spelling of the same computation works:

    (defn sums [] : (Option int)
      (do-m x (half 20) y (half x) (some (+ x y))))    ; => 15

`:when` clauses hit the same wall via `.empty`, which is also return-position.

## Root cause

`stdlib/macros.tur:79`:

    (list .bind coll (list fn `[~var] (list .pure (list do ~@body))))

The `.pure` is emitted bare. Return-position dispatch needs an expected type,
and a `fn` body supplies none. The macro predates there being more than one
`Applicative` instance in the auto-loaded set -- `tests/fixtures/hkt-for-
comprehension` still passes because it declares its own single-instance
`TestApplicative` and never loads the stdlib ones, so the unique-instance
fallback covers it. That fixture cannot catch this regression by construction.

## Fix directions

1. Make the macro thread the monad. `for` could take an optional witness --
   `(for @Option [x ...] body)` -- and emit `.pure @Option`. Note that `@Type`
   alone is not sufficient today: `(pure @Option 42)` still reports
   *"cannot infer type for return-directed method 'pure'; add a type
   ascription"*, so the witness path needs to start resolving the return type
   before it can back this.
2. Or have the macro ascribe: derive the type from `coll` and emit
   `(:: (.pure ...) <T>)`. This needs the element type at macro-expansion time,
   which is the harder half.
3. Or drop `.pure` from the desugaring entirely and require the body to produce
   an already-wrapped value, i.e. make `for` a thin alias for `do-m`. Cheapest,
   and loses the comprehension ergonomics.
4. Regardless: add a fixture that exercises `for` against the **stdlib**
   `Option` instances rather than a bespoke single-instance class, so this class
   of regression is visible.

## Workaround

Use `do-m`. `docs/guides/effects-vs-monads.md` says so in the sharp-edges
section and does not use `for` in any example.

## Resolution (2026-08-13)

Fixed, and by none of the four fix directions -- because the root cause section
is wrong about the mechanism, in a way that made all four look necessary.

### The expected type was there the whole time

The report says: "Return-position dispatch needs an expected type, and a `fn`
body supplies none." That is not what happens. Bare `pure` in the very same
position resolves fine:

```
(bind (half 20) (fn [x] (pure x)))     ; works -- prints 10
(.bind (half 20) (fn [x] (pure x)))    ; works
(.bind (half 20) (fn [x] (.pure x)))   ; AMBIGUOUS -- what `for` emits
(bind (half 20) (fn [x] (.pure x)))    ; AMBIGUOUS
```

The discriminator is `.pure` versus `pure`, not the surrounding context. `bind`'s
signature pins the lambda's result type, so the expected type reaches the body;
the **dot-dispatch path simply was not consulting it**.

Which makes the underlying problem a category error rather than a missing
channel: `.m` means "dispatch on the first argument", and for a return-directed
method the first argument is the *payload*, not the class type. `(.pure 42)`
asks the compiler to pick an `Applicative` by looking at `42`. With one instance
in scope the unique-instance fallback covers that up; with the two auto-loaded
ones (`Schema`, `Option`) it is ambiguous, and the ambiguity is reported from
inside `stdlib/macros.tur` where no caller can annotate.

### Fix

In `elab_typeclasses.c`, the dot-dispatch ambiguity branch now asks the
return-directed dispatcher before giving up: if an expected type is present and
the method is return-directed, delegate to `elab_try_return_dispatch`. The dot
form and the bare form have the same argument list, so the delegation is direct.

Placed in the ambiguity branch specifically, so it can only affect programs that
already hard-error, and gated on an expected type being present so a genuinely
unresolvable case still gets the ambiguity message rather than "cannot infer
type for return-directed method", which would be the worse of the two for the
same program.

`stdlib/macros.tur` is unchanged, and no existing fixture changed.

### Two other routes, and why they are not what landed

Both were implemented and backed out, and the reasons are worth keeping:

- **Emit bare `pure`/`empty` from the macro** (close to fix direction 3, and a
  one-line change). It fixes the stdlib case and *breaks* the bespoke ones:
  `hkt-for-comprehension` and `hkt-for-comprehension-vec` declare a
  single-instance `TestApplicative` whose methods return `: int`, so bare
  `pure` has nothing to infer from and reports "cannot infer type for
  return-directed method". Neither spelling works for both corpora, which is
  why the fix belongs in dispatch rather than in the macro.
- **Relax the unique-instance fallback in return-directed dispatch** -- it is
  gated to instances whose head is the function arrow, and `n_impl == 1` already
  guarantees uniqueness, so the arrow condition looks redundant. It is not:
  removing it makes `errors/rt-return-dispatch-unascribed` pass, and that
  fixture exists to pin "an unascribed return-dispatched call must demand an
  ascription" for a single-instance `Default` class. The gate encodes a
  deliberate decision, not an oversight.

### Coverage -- fix direction 4, which was the load-bearing one

`tests/fixtures/for-comprehension-stdlib-option` exercises `for` against the
auto-loaded stdlib `Option`: two binders, a short-circuiting binder, and `:when`
both ways (`:when` goes through `.empty`, return-directed for the same reason).

The report is exactly right that the existing fixtures "cannot catch this
regression by construction" -- they declare their own single-instance class and
never load the stdlib ones, so the unique-instance fallback covers them however
the multi-instance path behaves. Verified against a deliberately-reverted build:
the new fixture reports `ambiguous method dispatch: '.pure'`, twice.

### Also corrected

`docs/guides/effects-vs-monads.md`'s sharp-edges section said `for` "is
currently caught by this ... **Use `do-m` instead**". That is no longer true, and
its explanation carried the same wrong mechanism as this report's root-cause
section. Rewritten to describe the dot-form category error and to show `for`
working.

### Verification

`tests/run.sh`: 2596 passed, 0 failed. `tests/run-turi.sh`: 1782 passed, 0
failed. The report's repro prints `15` on both engines, and `hkt-for-comprehension`
/ `hkt-for-comprehension-vec` still pass unchanged.
