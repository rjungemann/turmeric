---
title: Lazy solution streams for stdlib/logic.tur (LS)
category: Planning
description: The fix for logic-streams-are-strict is written, works, and cannot land -- it is blocked on a codegen cast, and the block is the interesting part.
---

# Lazy solution streams (LS)

**Status:** implemented and verified, **blocked on a compiler fix**, reverted
from `stdlib/logic.tur` pending it. The working change is checked in as
[lazy-streams.patch](lazy-streams.patch) -- apply with
`git apply docs/upcoming/lazy-streams.patch`.

Fixes [logic-streams-are-strict](../reported/logic-streams-are-strict.md).
Blocked by [closure-in-defdata-field](../reported/closure-in-defdata-field.md).

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

## Order

1. Fix `closure-in-defdata-field` -- at minimum the cast, ideally also making
   `:fn` fields either work or be a compile error rather than a runtime crash.
2. Apply this patch; the 9 fixtures should go green with no other change.
3. Add fixtures for the new behaviour: an infinite relation under `run-logic n`,
   and `zzz` in a recursive relation. There are none today -- `logic.tur`'s
   whole coverage is 8 fixtures that only exercise finite goals.
4. Rewrite `docs/guides/tur-logic-guide.md`'s "Interleaving search" section:
   the correction currently there says `mplus-i` cannot work, which stops being
   true once `StInc` exists. `st-append` IS the interleaving `mplus` after this
   patch, so the section becomes a description rather than a workaround.
5. Update `logic-streams-are-strict` to resolved and archive it.
