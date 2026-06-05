---
title: A self-recursive defn declared `: ptr<void>` has its recursive call inferred as `int` when wrapped inside `(defmodule ...)`; if-branch unifier then rejects the body
category: Reported
severity: medium
description: A self-recursive function declared to return `:ptr<void>` (e.g. an SF-pipeline fold over a Vec) elaborates cleanly when defined at file top level, but the same body wrapped inside a `(defmodule signal/X (export ...) <body>)` form produces TUR-E0001 with `else=int` on the recursive arm of an `if`. The recursive call site is being typed at `int` (presumably the fat-closure carrier) instead of the declared `:ptr<void>` return type, so the `if`-branch unifier sees `then=(fn ...)` vs `else=int` instead of two fat boxes.
---

# Recursion return type widens to `int` inside `(defmodule ...)`

## Summary

This `__chain-loop` function -- the standard Vec<typed fat closure>
fold shape for an effects-chain -- elaborates clean at file top level
but is rejected inside a `defmodule` wrapper:

```turmeric
(defn __chain-loop
  [effects  : int
   ^fat sig : (fn [float] #{} float)
   i : int
   n : int] : ptr<void>
  (if (>= i n)
    sig
    (__chain-loop effects
                  (__apply-sf (:: (vec-get effects i) :ptr<void>) sig)
                  (+ i 1)
                  n)))
```

Top-level: clean. Wrapped in `(defmodule signal/compose (export ...) <this>)`:

```
error: if branches have mismatched types: then=(fn [float] : float) else=int
 36 |   (if (>= i n)
 37 |     sig
 38 |     (__chain-loop effects
```

The `then` is the captured `^fat sig`'s declared type. The `else` is
the self-recursive call -- which should be the declared `:ptr<void>`,
not `int`. The PR #288 if-branch unifier accepts two fat-box arms
(both `:ptr<void>`); here the recursive arm widens to `int`, so the
unifier sees a function-type vs an integer and bails.

## Severity

Medium. The recursion pattern is the natural shape for
`effects-chain`, the rebuild plan's Phase 5 deliverable
([[tur-signal-rebuild-plan]]). A workaround (ascribe the recursive
call to `:ptr<void>` at the call site) exists, but it's a surprising
defmodule-specific tax. Spice authors who export a typed-fold helper
will hit this.

## Observed vs. expected

### Observed

Top-level (the shape PR #288 added as a fixture): elaborates clean.

```turmeric
(defn __chain-loop [...] : ptr<void>
  (if (>= i n) sig (__chain-loop ... ...)))
```

Wrapped:

```turmeric
(defmodule signal/compose
  (export effects-chain)
  (defn __chain-loop [...] : ptr<void>
    (if (>= i n) sig (__chain-loop ... ...))))
```

TUR-E0001 with `else=int` on the recursive arm.

### Expected

The recursive call should be typed at the declared return type
(`:ptr<void>`) regardless of whether the defn is wrapped in a
`defmodule`. PR #288's HRT5 fix (early-update of arg_fat/result_fat on
the forward binding) appears to skip the inside-defmodule path.

## Reproducer

`../turmeric-spices/spices/signal/src/signal/compose.tur` before the
workaround was applied. The smaller standalone repro:

```turmeric
(defmodule probe/loop
  (export loop)
  (defn loop [i : int n : int] : ptr<void>
    (if (>= i n)
      (:: 0 :ptr<void>)
      (loop (+ i 1) n))))
```

`tur check` on the above rejects the recursive arm with `else=int`.
The same defn at top level passes.

(I haven't reduced this further but the symptom matches: declared
return is `:ptr<void>`, recursion arm is typed `int`.)

## Proposed fix direction

The HRT5 forward-binding update in `elab_fns.c` (PR #288 fix #1)
populates the forward binding with `arg_fat`/`result_fat`/return-type
info before the body is elaborated. The defmodule elaboration path
likely walks defns through a different gate where the forward binding
either (a) hasn't been populated yet at the body-elaboration step, or
(b) gets reset/shadowed by the module-scope environment. The result
is the recursive call site sees only the un-typed carrier (`int`).

Two possible directions:

1. Plumb the HRT5 early-update through the module-scope elaboration
   pass too, so the inside-defmodule forward binding carries the same
   info as the top-level one.

2. Add a fixture mirroring the existing
   `tests/fixtures/vec-typed-fat-closure-readback/` but with the
   defns wrapped in `(defmodule probe/X (export ...) ...)`. That locks
   in the symmetric behavior and prevents regression of either path.

## Workaround in place

`../turmeric-spices/spices/signal/src/signal/compose.tur` ascribes
the recursive call to `:ptr<void>`:

```turmeric
(:: (__chain-loop ... ...) :ptr<void>)
```

That forces the `if`-branch unifier to see two fat boxes. Library-side
`tur check` is clean with this workaround.

## Validation of a fix

- Drop the `(:: ... :ptr<void>)` ascription on the recursive call in
  `compose.tur`; module still checks clean.
- The proposed `tests/fixtures/vec-typed-fat-closure-readback-defmodule/`
  fixture mirrors the existing fixture but inside a defmodule and
  PASSes.
- No regression of the original `vec-typed-fat-closure-readback`
  fixture (orthogonal issue but worth running together).

## Related

- PR #288 (commit `6a5f149a`) -- shipped the top-level fix; the
  defmodule path isn't covered by that PR's fixture.
- [[vec-typed-fat-closure-readback-fixture-regressed-codegen]] --
  separate codegen-side regression on the same fixture; orthogonal.
- [[tur-signal-rebuild-plan]] -- Phase 5 surfaced this while wrapping
  `signal/compose.tur` inside its own defmodule for `:refer` imports
  to work.
