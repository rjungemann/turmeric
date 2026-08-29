# Backtrackable State Guide

`stdlib/trail.tur` gives you mutable cells whose writes **undo on demand**. You
take a mark, mutate freely, and then either throw the mutations away or keep
them. This is the WAM / constraint-logic-programming shape, and it is what a
search loop wants instead of threading a persistent structure through every
step.

It is autoloaded, so there is nothing to import and no flag to pass.

```turmeric
(defn main [] : int
  (let [c (bt-cell-new 10)
        m (bt-mark)]
    (bt-set! c 20)
    (println (bt-get c))    ; 20
    (bt-undo-to! m)
    (println (bt-get c))    ; 10 -- the write is gone
    (bt-cell-free c)
    0))
```

## Why a trail rather than a persistent structure

Because undoing recorded state is cheaper than rebuilding it, and because it can
be **asymmetric**.

Measured against `stdlib/logic.tur`'s real persistent `Subst` (an `SBind` chain
walked by `logic-walk`), an indexed trailed substitution wins at every size, with
no crossover:

| bindings | persistent ns/op | trailed ns/op | speedup |
|---:|---:|---:|---:|
| 1 | 209.5 | 18.7 | 11.2x |
| 8 | 194.1 | 11.5 | 16.8x |
| 64 | 369.9 | 20.7 | 17.9x |
| 512 | 782.2 | 22.7 | 34.4x |

The expectation going in was that a persistent list would win at small `n` -- it
is O(1) to extend and free to backtrack -- and lose only once its O(n) lookup
began to dominate. It never wins.

The asymmetry matters more than the constant. A search that backtracks usually
wants to discard *most* of what it did and keep a little: a solver drops the
trail above a backjump point but keeps the clause it learned. A persistent
snapshot restores everything or nothing. A trail lets you say which.

## The three opt-outs

Backtracking is asymmetric, so the design turns on being able to **opt a cell
out**, at three granularities. Pick the narrowest one that fits.

| granularity | mechanism | for |
|---|---|---|
| per cell, forever | `g-cell-new` | activity scores, statistics, a learned-clause store |
| per write | `with-untrailed` | one write inside an otherwise-trailed cell that must survive |
| per level | `bt-commit-to!` | promote this level's writes and drop the level |

`BtCell` and `GCell` are **distinct types**, so opting out is visible in the
signature of anything that touches the cell rather than being a flag someone
forgets to pass.

```turmeric
(let [g (g-cell-new 5)      ; never trailed
      c (bt-cell-new 1)     ; trailed
      m (bt-mark)]
  (g-set! g 6)
  (bt-set! c 2)
  (println (bt-depth))      ; 1 -- only the BtCell reached the trail
  (bt-undo-to! m)
  (println (g-get g))       ; 6 -- survived
  (println (bt-get c)))     ; 1 -- restored
```

### `bt-commit-to!` escapes the whole scope, not one level

Committing means "promote to level 0". It discards the undo information rather
than merging it into the enclosing level, so a committed write survives **every**
enclosing `bt-scope` too. That is deliberate -- it is how a learned clause
outlives a backjump -- but it is the opposite of what "commit into the parent"
suggests to a Prolog ear.

## Scopes

`bt-scope` is the bracket form of `bt-mark` / `bt-undo-to!`. Prefer it: the
halves are easy to leave unpaired on an early return, and the bracket cannot be.

```turmeric
(bt-scope (fn [] (do (bt-set! c 99) (bt-get c))))   ; => 99, and c is restored
```

The **value** survives; the trailed **writes** do not. Returning something that
points into state the scope just undid is the caller's problem -- this is a
search primitive, not a memory-safe region.

Two caveats:

- **A panic inside the body skips the undo**, leaving the trail at the inner
  level. There is no unwind protection to hang the restore on. `with-untrailed`
  is worse in the same way: a panic there leaves trailing paused for everything
  after it, so later writes silently stop being undoable. Do not put fallible
  work in a `with-untrailed` that a `bt-scope` could hold instead.
- **Free cells outside the scope that wrote them.** A trail entry still pointing
  at a freed cell dangles until the next undo, and the failure surfaces inside
  `bt-undo-to!` rather than at the free.

## Why a thousand writes cost one trail entry

Every cell carries the level at which it was last trailed. A write records only
when that stamp is below the current level, then bumps it -- so a cell written
repeatedly inside one level costs **one** entry, not one per write. This is what
makes the primitive affordable in a search loop, and the entry count is the only
place it is observable:

```turmeric
(let [c (bt-cell-new 0)
      m (bt-mark)]
  (spin c 500)              ; 500 writes
  (println (bt-depth))      ; 1, not 500
  (bt-undo-to! m))
```

## Write-once cells

`bt-lvar-new` is the logic-variable shape: unbound until bound, bound once per
level, and undo returns it to unbound. `bt-set!` **refuses** a second binding
rather than overwriting -- binding twice is a caller bug, not a silent
overwrite -- and returns `false` when it does.

## Continuations and the trail

A continuation captures **control, not state**. Re-enter one and you resume a
computation whose trail levels may already be gone.

**The type checker prevents most of this outright.** `Mark`, `BtCell` and `GCell`
have no `Clone` instance, and `cloneable-shift` requires `Clone` on every
captured binding, so a multi-shot continuation **cannot close over a trail
handle**. That is a `TUR-E0014` at compile time.

Where a mark does get through -- laundered via a plain `:int`, or via inline-C --
a mark carries a *generation* as well as a level, so `bt-undo-to!` on a spent
mark returns `false` instead of unwinding whichever level now occupies that
index. Always check the result if the mark could be stale:

```turmeric
(let [m (bt-mark)]
  (bt-set! c 2)
  (println (if (bt-undo-to! m) 1 0))   ; 1 -- works
  (println (if (bt-undo-to! m) 1 0)))  ; 0 -- refused, and changed nothing
```

Unwinding *past* a scope is fine: `escape`, an abortive `shift0`, or a panic
unwinds through and the undo runs like any scope-exit action.

**Serializing a continuation inside an open scope is refused.** The undo
information is process-local and does not travel with the blob, so the
serialization aborts naming the scope depth and the outstanding write count.
Serialize outside the enclosing scope, or `bt-commit-to!` the level first.

## The search driver

`stdlib/backtrack-dfs.tur` builds depth-first search on top of the trail. A
**goal** is `(fn [(fn [] bool)] bool)`: it receives a success continuation `k`
and calls it once per solution, with bindings live in the trailed cells for
exactly the duration of that call.

```
k returns:       true = keep searching     false = cut the whole search
a goal returns:  true = exhausted normally false = a stop is propagating out
```

Combinators: `dfs-succeed`, `dfs-fail`, `dfs-and`, `dfs-or`, `dfs-guard`,
`dfs-set`, `dfs-choose-int`, `dfs-solve`.

**Solutions escape by reification, not by reference.** When `k` runs, the answer
exists only as the current cell values; the moment the search backtracks past it,
they are undone. Whatever you want to keep, `k` must copy out -- into a vec, a
`GCell`, or by printing -- before returning. Returning a `BtCell` handle and
reading it later yields the restored pre-search value, by design.

### Which driver to reach for

Against `stdlib/backtrack.tur`'s list monad on N-queens, **the list monad is
4-11% ahead at N=4..7** -- the trail's 11-34x was against persistent-state
*lookup*, and enumeration is a different fight where the driver pays a fat
dispatch per combinator layer per node. What the driver buys is **scale and
reification**: the list path's packed-int board is structurally capped at N=7,
while the driver runs N=8/9/10 and can read any solution out of live cells.

So: list monad for small, fixed enumeration; the trailed driver when the state is
large, when you need to reify answers, or when the search is the inner loop of
something that also wants the asymmetric keep-what-you-learned behavior.

### Two spellings the checkers dictate

Worth knowing before extending the driver:

- The combinators use the `bt-mark` / `bt-undo-to!` **halves**, not the
  `bt-scope` bracket, because wrapping `(g1 k)` in a scope thunk *captures* `k`,
  and capturing a fat closure moves it (`TUR-E0005`). Calling is non-consuming;
  capture is not.
- Recursion is written pass-`k`-through (`dfs-choose-go`) rather than
  goal-returning, because a self-recursive call whose fn-typed result feeds a
  `^fat` parameter hits a known
  [codegen bug](https://github.com/rjungemann/turmeric/blob/main/docs/reported/self-recursive-fn-returning-call-into-fat-sink.md);
  the same call through a one-line forwarder works.

## Not available under `--interpret`

Every binding here is inline-C over `src/runtime/trail.c` and none is registered
as a turi native, so the tree-walker reports `unknown name 'bt-cell-new'`. The
compiled path is the supported one. Tracked in
[trail-tur-has-no-turi-natives](https://github.com/rjungemann/turmeric/blob/main/docs/reported/trail-tur-has-no-turi-natives.md).

## See also

- [Backtracking Guide](backtracking-guide.md) -- the list-monad search surface.
- [Logic Programming Guide](logic-programming-guide.md) -- relational search and `Subst`.
- [Delimited Control Operators Guide](delimited-control-operators-guide.md) -- `shift`/`reset`, `call/cc*`.
- [Solver extension plan](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/solver-extension-plan.md) -- design rationale, sections 3.2-3.5.
