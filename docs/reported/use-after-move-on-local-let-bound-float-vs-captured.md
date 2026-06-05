---
title: TUR-E0005 use-after-move fires on a local let-bound :float used in (if (< x v) v ...) but not on the equivalent captured-let binding
category: Reported
severity: low
description: A `let`-bound `:float` used twice in a single `if` (once in the predicate, once as the then-branch value) trips TUR-E0005 "binding 'v' was moved" when v is bound in an inner `let` next to the closure body. The identical use pattern on a `:float` bound in the *outer* (captured) `let` -- the one whose value the inner closure captures -- is accepted. `:float` is a copy type; neither use should be a move.
---

# `use-after-move` fires asymmetrically on local vs captured `:float` let bindings

## Summary

A symmetric hard-clip SF written naturally as

```turmeric
(defn hard-clip [limit :float]
  (let [lv limit]                                  ;; outer (captured) let
    (fn [^fat sig : (fn [float] float)]
      (fn [t :float] :float
        (let [x (sig t)
              nv (- 0.0 lv)]                       ;; inner (local) let
          (if (< x nv) nv                          ;; use #1, use #2
            (if (< lv x) lv x)))))))               ;; lv used the same way; OK
```

elaborates to TUR-E0005 on the inner `nv`:

```
error [TUR-E0005]: use-after-move: binding 'nv' was moved and cannot be used again
 98 |           (if (< x nv) nv
    |                        ^^
note: moved here
 98 |           (if (< x nv) nv
    |                    ^^
```

The captured `lv`, used in the exact same `(if (< _ lv) lv _)` pattern
one line down, is fine. Pulling the `(- 0.0 lv)` expression out of the
inner `let` and re-evaluating it twice inline at each use site (no
binding):

```turmeric
(let [x (sig t)]
  (if (< x (- 0.0 lv)) (- 0.0 lv)                  ;; OK
    (if (< lv x) lv x)))
```

also elaborates clean. So the issue is specifically the local
let-binding step, not the underlying use pattern.

## Severity

Low. Workarounds are obvious (inline the expression; lift the value to
an outer/captured let). But the asymmetry is surprising and the error
text ("was moved") is misleading for a copy-type primitive.

## Observed vs. expected

### Observed

Local `:float` let binding flagged as moved when used twice in an `if`
predicate + then-branch.

### Expected

`:float` is a copy type. Two reads should both be copies; neither
should be a move. The captured-let case (same pattern, different
binding site) demonstrates this is the elaborator's intended behavior;
the local-let case appears to be a missed copy-type check.

## Reproducer

```turmeric
(defn hard-clip [limit :float]
  (let [lv limit]
    (fn [^fat sig : (fn [float] float)]
      (fn [t :float] :float
        (let [x (sig t)
              nv (- 0.0 lv)]
          (if (< x nv) nv
            (if (< lv x) lv x)))))))
```

`tur check` rejects with TUR-E0005 on `nv`.

If `nv` is instead bound at the outer `let` next to `lv`:

```turmeric
(let [lv limit nv (- 0.0 limit)] ...)
```

the same use pattern elaborates clean.

## Proposed fix direction

In the linear/affine analysis pass:

1. Verify the copy-type rule fires for let-bound primitives the same
   way it does for captured-let primitives. The two paths should hit
   the same TypeKind-based "is copy?" gate.
2. If the divergence is intentional (e.g. local let bindings track
   liveness more strictly than captured ones), the error text should
   say so -- "moved" implies a value was consumed, which is misleading
   for `:float`.

A quick smoke: add a `tests/fixtures/use-after-move-float-let-vs-captured/`
that exercises both shapes (captured-let succeeds, local-let succeeds)
to lock down the symmetric behavior.

## Validation of a fix

- The natural hard-clip body, with `nv` bound locally next to `x`, is
  accepted by `tur check`.
- An asymmetry-probe fixture passes both shapes leak-clean.

## Workaround in place

`../turmeric-spices/spices/signal/src/signal/shaper.tur`'s `hard-clip`
inlines `(- 0.0 lv)` twice rather than caching it. Functionally
identical, redundant arithmetic at runtime, slightly less readable
source. See the commit message for the module split for context.

## Related

- [[tur-signal-rebuild-plan]] -- surfaced this while writing `shaper.tur`.
