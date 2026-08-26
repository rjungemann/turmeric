---
title: Lazy solution streams for stdlib/logic.tur (LS)
category: Planning
description: The fix for logic-streams-are-strict is written, works, and cannot land -- it is blocked on a codegen cast, and the block is the interesting part.
---

# Lazy solution streams (LS)

**Status:** LANDED 2026-08-26, in `stdlib/logic.tur`. The patch file that
carried it while it was blocked is gone; the change is in the module.

Fixed [logic-streams-are-strict](logic-streams-are-strict.md). The blocker,
[closure-in-defdata-field](../reported/closure-in-defdata-field.md), was fixed
first -- see step 1 below.

## What the patch does

Canonical miniKanren, nothing invented:

- **`(defopaque StThunk :ptr<void>)` and a third `Stream` variant,
  `(StInc :StThunk)`** -- the immature stream. The carrier is `:ptr<void>` and
  not `:fn` deliberately; a `:fn` field segfaults on a capturing closure (see
  the blocking report). This is the route `Goal` already takes.
- **`st-force` / `st-pull`** -- force one step, and force until mature. Every
  consumer pulls before looking at a head.
- **`st-append` swaps on immature**: `mplus (StInc t) ys` becomes
  `StInc (fn [] (mplus ys (force t)))`. That one swap is the whole of fair
  interleaving, and it only means anything because the result is *itself*
  immature -- which is exactly what the guide's old strict `mplus-i` missed.
- **`st-bind` defers through immature** rather than forcing its argument.
- **`zzz`, a macro** -- delays goal CONSTRUCTION. A function cannot do this: it
  would evaluate its argument at the call site, which is the divergence it
  exists to prevent.

## What it buys, measured

| | before | after |
|---|---|---|
| `(defn nats [] (disjoined (succeed) (zzz (nats))))`, `run-logic 1` | **SIGSEGV** during goal construction | returns 1 solution |
| same, `run-logic 5000` | SIGSEGV | 5000 solutions |
| 1 + 100 + 5000 solutions off an infinite relation | impossible | **0.014 s total** |

## What it costs, honestly

**Fair interleaving makes some first solutions more expensive, and the patch
does not hide that.** A goal whose only solutions sit at depth *d* of a binary
disjunction tree (`wide d` in the probes) went from 0.464 s to 3.524 s at
d=18 for `run-logic 1`: depth-first found the leftmost leaf in O(d), and
interleaving explores the tree breadth-first to get there.

That is the standard miniKanren trade and it is the right one -- DFS is fast on
the goals it terminates on and diverges on the ones it does not -- but it is a
regression for that shape and should be stated when this lands, not discovered.

## Why it cannot land yet

The suite has a cc-warning gate, and the patch trips it on **9 shipped
fixtures** (`logic-*`, `hkt-stdlib-logic-instances`), all with
`emitted C pointer/integer warning`.

`ctor_StInc` takes `int64_t` -- correct, and deliberate: an opaque "stays the
int64 carrier" by design (`types.h:353`). The argument is a `void *` closure
pointer, and no cast is inserted, so the emitted C warns.

The ctor path in `emit_expr.c:~6280` **already has** the carrier-cast logic for
exactly this (`slot_is_i64 && arg_is_ptr` -> `(int64_t)(intptr_t)(...)`). It
does not fire because it can only determine the argument's C type in three
cases: an already-cast string, a `(T *)(intptr_t)` string, or an `EX_VAR` whose
spec type resolves. A closure argument is none of those -- it emits as a
compiler temp (`void *__t82 = __t80;`) whose type the logic cannot see.

**So the fix is to widen that type determination, not to add a new cast.** The
narrowness looks deliberate (there is a comment warning that a blanket cast
would paper over a mis-selected monomorph), so widening it wants care and a
snapshot diff -- which is why this was not done at the tail of the session that
found it.

## What actually happened

1. **Fixed the blocker first.** Two predicates at the ctor-argument seam in
   `emit_expr.c`: `field_is_carrier` extended to opaque fields (an opaque is a
   named int64 carrier with its base erased), and `is_ptr_like` extended to an
   ascription-stripped `EX_CLOSURE` (a closure resolves to the opaque but
   lowers to a pointer). Zero drift across 147 snapshots; mutation-verified by
   reverting it and watching the `logic-*` fixtures go red again.
2. **Applied the patch.** The 9 fixtures went green with no other change, as
   predicted.
3. **Added `tests/fixtures/logic-lazy-infinite`** -- an infinite relation under
   `run-logic 1 / 5 / 500`, plus a finite goal to pin that nothing else moved.
   `logic.tur` previously had no coverage of either.
4. **Rewrote the guide's "Interleaving search" section.** It no longer tells
   readers to hand-write `mplus-i`; `st-append` is the interleaving `mplus`, and
   the section explains why the old hand-written version could never have
   worked (it swapped, but built a strict `StCons`).
5. **Archived `logic-streams-are-strict`.**

**Still open:** a `:fn` field in a `defdata` still segfaults on a capturing
closure. The lazy-stream work routes around it via `defopaque :ptr<void>`, which
is why it could land, but the underlying hole is unchanged -- see
[closure-in-defdata-field](../reported/closure-in-defdata-field.md).
