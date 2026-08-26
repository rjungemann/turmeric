# `stdlib/logic.tur`'s solution stream is strict, so `run-logic n` bounds results but not work

**Severity:** medium. Not a wrong answer -- a missing property. miniKanren's
defining characteristic is that `run 1` over an infinite relation terminates;
here it cannot, because the stream type has no thunk and the goal combinators
have no delay. Three shipped docs describe behaviour the implementation cannot
have.

**Status:** RESOLVED 2026-08-26. `stdlib/logic.tur` gained the immature stream
constructor `(StInc :StThunk)`, `st-force` / `st-pull`, a swapping `st-append`,
a deferring `st-bind`, and the `zzz` delay macro.

`(defn nats [] (disjoined (succeed) (zzz (nats))))` under `run-logic 1` used to
SIGSEGV while the goal was being built; it now returns, and 1 + 100 + 5000
solutions off that infinite relation take 0.013 s. Regression fixture:
`tests/fixtures/logic-lazy-infinite`.

Landing it required a compiler fix first --
[closure-in-defdata-field](../archive/closure-in-defdata-field.md), also
resolved -- because the emitted C tripped `tests/run.sh`'s cc-warning gate on 9
shipped fixtures.

**Carried forward:** fair interleaving makes some first solutions more
expensive. A goal whose solutions all sit at depth *d* of a binary disjunction
tree went from 0.464 s to 3.524 s at d=18 for `run-logic 1` -- depth-first found
the leftmost leaf in O(d), interleaving reaches it breadth-first. That is the
standard miniKanren trade and the right one (DFS is fast on the goals it
terminates on and diverges on the rest), but it is a real regression for that
shape.

Found while asking whether `logic.tur` has a region boundary an arena could
reclaim at ([the arena thread](../archive/history/per-entry-arena-gate.md)).
It does not, and this is the more consequential reason why.

## 1. `Stream` has no immature/thunk constructor

```turmeric
(defdata Stream :copy (StNil) (StCons :Subst :Stream))
```

Canonical miniKanren has a fourth constructor -- `Inc`/immature, a
`(-> Stream)` thunk -- and it is the whole mechanism behind lazy enumeration
and fair interleaving. Without it every combinator is strict:

```turmeric
(defn st-append [xs : Stream ys : Stream] : Stream
  (match xs (StNil) ys (StCons v rest) (StCons v (st-append rest ys))))

(defn st-bind [xs : Stream f : fn] : Stream
  (match xs (StNil) (StNil)
            (StCons v rest) (st-append (:: (f v) :Stream) (st-bind rest f))))
```

`st-bind` applies `f` to **every** solution before returning, and `st-append`
rebuilds the entire spine. So `apply-goal` materialises the whole search space.

## 2. `run-logic n` truncates the result, after doing all the work

```turmeric
(defn run-logic [A] [n : int g : (Goal A)] : Stream
  (let [init (subs-empty)] (st-take n (apply-goal (:: g (Goal int)) init))))
```

`apply-goal` runs to completion; `st-take` then keeps `n`. Measured with a goal
having 2^d solutions, asking for exactly one:

| depth | solutions | `run-logic 1` wall |
|---:|---:|---:|
| 10 | 1,024 | 0.016 s |
| 12 | 4,096 | 0.015 s |
| 14 | 16,384 | 0.022 s |
| 16 | 65,536 | 0.124 s |
| 18 | 262,144 | 0.464 s |

16x the solutions costs 21x the time for a query that returns one. Repro:

```turmeric
(load "stdlib/logic.tur")
(defn wide [n : int] : (Goal int)
  (if (<= n 0) (succeed) (disjoined (wide (- n 1)) (wide (- n 1)))))
(defn main [] : int (println (st-length (run-logic 1 (wide 18)))) 0)
```

## 3. A recursive relation SIGSEGVs before the search starts

There is no `Zzz`/delay form, and `disjoined` takes its goals strictly, so a
self-recursive relation diverges while the goal is being *built*:

```turmeric
(defn nats [] : (Goal int) (disjoined (succeed) (nats)))
(defn main [] : int (println (st-length (run-logic 1 (nats)))) 0)
```

```
Segmentation fault    (exit 139)
```

No diagnostic, no stack-depth message. This is the shape every non-trivial
miniKanren relation has -- `appendo`, `membero`, any recursive predicate -- so
what is missing is not an optimisation but the ability to write the programs the
library is named for.

## 4. Three docs describe behaviour that cannot happen

`docs/guides/tur-logic-guide.md` "Interleaving search" offers `mplus-i` and says
to use it "when you need **complete enumeration of infinite search spaces**":

```turmeric
(defn mplus-i [xs : Stream ys : Stream] : Stream
  (match xs (StNil) ys (StCons v rest) (StCons v (mplus-i ys rest))))
```

This is strict too -- `StCons`'s second field is evaluated before the cell is
built -- so it changes the ORDER of a finite enumeration and nothing else. It
cannot enumerate an infinite space, because no infinite `Stream` value exists.
Interleaving without laziness is not the miniKanren `mplus`; the swap is only
half the trick, and the half that is missing is the half that matters.

The same guide's "Tabling / memoisation" section opens "For recursive relations
that diverge under depth-first search" -- they do not diverge under search, they
crash during construction (section 3). And `logic.tur`'s own module docstring
advertises "the solution stream (the list / backtracking monad)"; it is the list
monad, and the backtracking monad it is standing in for needs the thunk.

## Why this matters beyond correctness

It is also the allocation story. The
[ADT-allocation report](multi-variant-adts-always-heap-allocate.md) measured
~85% of executed instructions inside `malloc` on `logic.tur`'s shapes. A strict
stream allocates a `Subst` chain for **every** solution in the search space
whether or not the caller wants it -- so a large part of that number is eager
enumeration, not the per-`defdata` boxing the report attributes it to. No arena
and no by-value lowering changes the asymptotics here; laziness does.

## Fix directions

1. **Add the immature constructor.** `(StInc :fn)` holding a `(-> Stream)`,
   with `st-append`/`st-bind`/`st-take` forcing one step at a time and
   `st-append` swapping arguments on `StInc` (which is where interleaving comes
   from for free). This is the canonical design and it makes 1-3 go away
   together.
2. **Add a delay form** so `disjoined` can take unevaluated goals -- the `Zzz`
   macro in the miniKanren literature. Without it section 3 stands even with a
   lazy stream, because the divergence is in goal construction.
3. **Correct the guide** either way. The `mplus-i` claim is wrong today and
   would still be incomplete after (1) unless it is presented as the `StInc`
   swap rather than as a drop-in replacement for `mplus`.

Sequencing note: (1) and (2) have to land together to be worth anything -- a
lazy stream whose goals still diverge at construction time buys nothing for the
recursive relations that motivate it.
