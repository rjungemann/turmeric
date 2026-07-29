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
