---
title: Future cannot be promoted to :affine while it aliases the Promise cell
category: Reported
description: Promise and Future are two opaque views over one shared FutureCell pointer; linearity's single-owner discipline was incompatible with handing out both views. RESOLVED by reference-counting the shared cell (fix direction 1), so Future is now :affine.
---

# Future cannot be promoted to `:affine` while it aliases the Promise cell

**Status:** RESOLVED -- `Future` is now `:affine`. Fixed via **direction 1**
(reference-count the shared cell); see the Resolution section at the end.

**Summary:** `stdlib/future.tur` models `Promise` and `Future` as two nominally
distinct opaque newtypes that are, at runtime, *the same `FutureCell *`
pointer*. Promoting `Future` to `:affine` (as the
[[stdlib-linearity-affinity-plan]] inventory calls for) was blocked because
linearity/affinity tracks single ownership, and the public API deliberately
hands out two owning handles to one cell.

**Severity:** expressiveness gap / design tension. Not a miscompile -- `Promise`
was promoted to `:linear` first (producer-end settle-exactly-once is sound),
and this report tracked the remaining `Future` inventory item until it was
resolved.

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

Direction 3 shipped first (Promise `:linear`, Future plain opaque). The
checked read end was then implemented via **direction 1** (refcount).

## Resolution (direction 1: reference-counted cell)

`stdlib/future.tur` now gives `FutureCell` a `refcount` field and promotes
`Future` to `:affine`:

- `promise-new` mints a cell with `refcount = 1`, handed out as a `Promise`.
- `future-handle [^borrow p : Promise] : Future` borrows the `Promise` and
  mints an *additional* `Future` over the same cell, bumping the count to 2
  (`__atomic_add_fetch`). This is the both-ends split; the producer keeps the
  `Promise`, the consumer gets the `Future`. (`future-of-cell` remains the
  ownership *transfer* form -- consume the `Promise`, hand back a `Future`,
  count unchanged -- for the read-only case.)
- `promise-fulfill` / `promise-fail` / `promise-free` / `future-cell-free` and
  `future-free` each drop one reference (`__atomic_sub_fetch`); whichever drop
  takes the count to 0 destroys the mutex/condvar and frees the cell. The two
  aliases therefore can no longer double-free it, and the decrement is atomic
  so a `Promise` fulfilled on one thread and a `Future` freed on another tear
  the cell down race-free.
- `Future` is `:affine` (not `:linear`): an uncollected future may be dropped
  (matching its fire-and-forget role) but cannot be used after `future-free`.
  The read accessors `future-get` / `future-done?` / `future-cancelled?` /
  `future-cancel` and every combinator (`future-map`/`-then`/`-race`/`-all2`/
  `-any2`/`-join`/`-with-timeout`) take their `Future` argument `^borrow`, so
  reads, cancellation, and combinator chaining never consume the input; only
  `future-free` consumes.

This also required a compiler fix: the call-site **move checker** poisoned a
`CK_MOVE` (`:affine`) argument even when the parameter was `^borrow`, so a
borrowing accessor could not read an affine handle and then have the caller
use it again (`TUR-E0005`). A `^borrow` parameter now suppresses the move-mark
(`src/compiler/elab_call.c`), the move-checker half of the borrow form. See
[[borrow-param-forwarding-drop]] for the sibling consumption-side fix.

### Validating the fix

- `bash tests/run.sh` green with leak detection on.
- Positive `tests/fixtures/future-linear` (fulfill + borrow-read + free under
  `-Xlinear`) and `tests/fixtures/future-split-free` (both ends explicitly
  released; the refcount yields exactly one teardown -- the historical
  cross-alias double-free is now memory-safe). Both run leak-clean.
- Negative `tests/fixtures/errors/future-linear-double-free` and
  `future-linear-use-after-free` prove that reusing a single `Future` handle
  after `future-free` is caught at compile time (`TUR-E0005`, the affine
  use-after-move). The cross-alias double-free is *prevented* by the refcount
  rather than diagnosed, since `promise-free p` and `future-free f` are each a
  single legal consumption of distinct handles.
