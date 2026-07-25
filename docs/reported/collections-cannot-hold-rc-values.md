# `vec` and `map` cannot hold `rc<T>` at all

**Severity:** medium (expressiveness hole; hard codegen error, not a miscompile)
**Status:** open
**Found by:** CG7 (gc-cycle-collection-followup-plan), while trying to write the
`RCK_OPAQUE` blind-spot fixtures

## Summary

Storing a reference-counted value in either built-in collection fails in
codegen:

```turmeric
(defstruct S :move [tag : int])
(defn main [] : int
  (let [a (rc/of (make-struct S 1))
        v (vec-of (rc/clone a))]     ;; or (hamt-of :k (rc/clone a))
    (println (rc/strong-count a)))
  0)
```

```
tur: emit: invalid EX_REINTERPRET rc -> int
```

Both `vec-of` and `hamt-of` fail identically. The value type-checks -- the
failure is at emission, where the element is reinterpreted into the collection's
`int64_t` carrier and `rc<T>` has no such reinterpretation.

## Why it matters

Two separate reasons.

**1. It is a plain expressiveness hole.** "A vector of shared handles" is an
ordinary shape -- a scene graph's children, a connection pool, an observer list.
Today the workaround is to erase the `rc<T>` to a raw handle and manage the
counts by hand, which is exactly the kind of `:int` type-erasure this codebase
otherwise rules out.

**2. It silently narrows the GC's documented blind spot.**
[docs/guides/gc-guide.md](../guides/gc-guide.md) says a cycle routed through an
`RCK_OPAQUE` payload -- "collection buffers such as a `vec` of `rc<T>`" -- is not
reclaimed, and the cycle-collection plan called for fixtures asserting that
non-collection. Those fixtures cannot be written: the shape does not compile.

So the blind spot is currently *narrower* than documented, but for a reason
nobody would want -- the hole is closed by rejection, not by tracing. If
collections ever do accept `rc<T>`, the blind spot opens up for real and the
fixtures become writable and necessary at the same moment.

Worth noting the neighbouring case is fine: a closure that **captures** an
`rc<T>` releases it correctly at scope exit (verified -- live block count returns
to 0 across a collection). The blind spot is not "anything opaque leaks."

## Root cause

Not fully traced. The error comes from the `EX_REINTERPRET` emission path
rejecting `rc -> int`; the collections store elements as an `int64_t` carrier,
so an `rc<T>` element needs either a real boxing step or an element-type-aware
storage path.

## Fix directions

Roughly in increasing order of work:

1. **Diagnose it properly.** Today it is a bare `tur: emit:` line with no span,
   which is a poor error for something a user hits by writing an ordinary
   program. A type-checker rejection with a source span and a "collections
   cannot yet hold `rc<T>`" message would at least be honest.
2. **Box the element.** Store `rc<T>` elements as their control-block pointer
   with the collection owning a strong reference -- taking a count on insert and
   releasing on removal/teardown. This is the shape the existing
   `RCK_EXISTENTIAL` / `RCEXP_RC` machinery already models.
3. **Make the collection walkable** so the cycle collector can trace through it
   (CG3 item 2). This is the part that closes the documented blind spot rather
   than dodging it, and it is only worth doing once (2) exists.
