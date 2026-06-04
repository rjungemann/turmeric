---
title: Future cannot be promoted to :affine while it aliases the Promise cell
category: Reported
description: Promise and Future are two opaque views over one shared FutureCell pointer; linearity's single-owner discipline is incompatible with handing out both views (promise-pair / future-of-cell), so Future stays a plain opaque until the shared cell is untangled.
---

# Future cannot be promoted to `:affine` while it aliases the Promise cell

**Summary:** `stdlib/future.tur` models `Promise` and `Future` as two nominally
distinct opaque newtypes that are, at runtime, *the same `FutureCell *`
pointer*. Promoting `Future` to `:affine` (as the
[[stdlib-linearity-affinity-plan]] inventory calls for) is blocked because
linearity/affinity tracks single ownership, and the public API deliberately
hands out two owning handles to one cell.

**Severity:** expressiveness gap / design tension. Not a miscompile -- `Promise`
*was* promoted to `:linear` in the same pass (producer-end settle-exactly-once
is sound). This report records why `Future` was deliberately left as a plain
`defopaque` so the remaining inventory item is not silently forgotten.

## Observed vs. expected

- **Expected (per plan):** `(defopaque Future :ptr<void> :affine)` so that
  dropping or cancelling a future is checked, and `future-free` is the single
  consumer.
- **Observed:** doing so makes `stdlib/future.tur`'s own helpers and the
  documented usage pattern un-typecheckable under `-Xlinear`, because a
  `Promise` and a `Future` are aliases of one pointer.

## Root cause

The shared cell is a single heap allocation viewed two ways
(`stdlib/future.tur`):

- `future-cell-new` / `promise-new` return a `Promise` (write end).
- `future-of-cell [cell : Promise] : Future` (`stdlib/future.tur:127`)
  *casts the same pointer* to a `Future` (read end).
- `promise-pair` (`stdlib/future.tur:138`) expands to
  `(list (promise-of-cell cell) (future-of-cell cell))` -- i.e. it uses the one
  `cell` **twice** to produce both a `Promise` and a `Future` to the same
  allocation.
- Both `promise-free` (`:198`) and `future-free` (`:265`) `free()` that one
  allocation.

Under linearity, a value must be consumed exactly once. The
producer/consumer split fundamentally wants **two** live owning handles to
**one** resource that is freed **once**. That is precisely the multi-owner
aliasing linearity rejects:

- `promise-pair` uses `cell` twice -> use-after-consume (`TUR-E0101`).
- If `Future` were affine and both `promise-free` and `future-free` consumed,
  the cell has two would-be consumers; the type system cannot express "free via
  exactly one of these two aliases."

`Promise` alone is promotable because the producer end genuinely is one-shot:
`promise-fulfill` / `promise-fail` settle it exactly once and the value is then
spent. The aliasing only bites the *read* end.

## Repro

Under `-Xlinear`, with `Future` promoted to `:affine`, the stdlib's own
`promise-pair` macro no longer type-checks (double use of `cell`), and:

```turmeric
;; flags: -Xlinear
(load "stdlib/future.tur")
(defn main [] : int
  (let [p (promise-new)]
    (let [f (future-of-cell p)]   ;; consumes p, yields a Future alias
      (promise-fulfill p 42)      ;; ERROR: p already consumed by future-of-cell
      (future-free f)))
  0)
```

The fulfill-then-read pattern that the API is designed around requires holding
both handles to the same cell at once, which single-ownership forbids.

## Proposed fix directions

1. **Split the shared cell into two allocations** with a refcount or an
   explicit ownership-transfer step, so `Promise` and `Future` own genuinely
   distinct resources and each is freed by exactly one consumer. Most invasive;
   changes the C representation.
2. **`unsafe-dup` escape hatch** (already anticipated in the plan's *Risks*
   section): expose `(future-of-cell-dup p)` that borrows the Promise and mints
   a Future alias whose `future-free` is a no-op (the Promise side owns the
   free). Keeps one real owner; the other handle is a non-owning view.
3. **Borrow-based read end:** keep `Future` non-linear (a plain opaque view) and
   require the `Promise` to be the single linear owner. This is the status quo
   after this pass and is arguably the cleanest: the producer is checked,
   reads are unrestricted, and `promise-free` is the single consumer.

Direction 3 is what shipped (Promise `:linear`, Future plain opaque). Revisit
1/2 only if checked read-end ownership is genuinely required.

## Validation of a future fix

- `bash tests/run.sh` green with leak detection on.
- A positive fixture exercising fulfill + read + free under `-Xlinear`.
- A negative fixture proving double-free across the Promise/Future alias is
  caught (the very bug class this would buy us).
