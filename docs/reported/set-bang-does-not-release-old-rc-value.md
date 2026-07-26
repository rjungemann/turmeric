# `set!` on an `^mut` binding holding `rc<T>` never releases the old value

**Severity:** high -- an ordinary, idiomatic loop leaks one rc block per
iteration. Acyclic, so the cycle collector cannot reclaim it either: the
overwritten blocks keep `strong_count > 0` forever and are simply lost.

Found while running a real-shape workload through the CG6 counters for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../upcoming/v1/gc-cycle-collection-followup-plan.md)
(CG8 item 3). The collector handled every *cyclic* structure in that workload
correctly (7940 blocks freed, 60 live at exit); the entire residue came from
this, on the acyclic half.

## Minimal repro

```turmeric
(defstruct Node :move [next : rc<Node>])
(defn null-rc-node [] : ptr<void>
  ```c
  return NULL;
  ```)
(defn live [] : int
  ```c
  extern uint32_t gc_all_blocks_count;
  return (int64_t)gc_all_blocks_count;
  ```)
(defn main [] : int
  (gc-enable!)
  (println (live))                     ; 0
  (let [^mut h (rc/of (make-struct Node (null-rc-node)))
        ^mut i 0]
    (while (< i 10)
      (set! h (rc/of (make-struct Node (null-rc-node))))
      (set! i (+ i 1))))
  (println (live))                     ; expected 0, actual 10
  0)
```

```
$ ./build/tur build repro.tur -o repro && ./repro
0
10
```

11 blocks are allocated (1 initial + 10 assignments). The binding goes out of
scope before the second `println`, so all 11 should be released. Exactly the 10
*overwritten* values survive -- the one still bound at scope exit is released
correctly, so scope-exit drop works and it is specifically the assignment path
that is missing its decrement.

## Scale

A `build-chain` loop of 4 assignments per round, 4000 rounds, under `GC_AUTO`:

| | collections | objects freed | live blocks at exit |
|---|---|---|---|
| acyclic chains only | 31 | **0** | **16000** |
| cyclic families only | 63 | 7940 | 60 |

16000 = 4000 rounds x 4 `set!`s, exactly. Nothing is reclaimed on the acyclic
path at all, and the collector correctly does not try -- these are not cycles.

## Root cause (direction, not confirmed)

The scope-exit drop is emitted and works; the assignment path is what lacks the
release. The fix is presumably to emit a `rc_strong_decrement` of the previous
value before storing the new one when the assigned binding's type is an owning
`rc<T>` (and the same question applies to `ref`/`lref`/`weak` bindings and to
`set!` on a *struct field* of owning type -- worth checking all four rather
than just the `^mut` local).

Care needed on self-assignment (`(set! h h)`) and on the case where the new
value borrows from the old one, e.g.
`(set! h (rc/of (make-struct Node (rc/clone h))))` -- the clone must be taken
before the old value is released, or the release frees the very block being
cloned. The original workload used exactly that shape, so it is not
hypothetical.

## Related

Not a collector bug, and not fixed by enabling the collector: these blocks are
acyclic with a positive strong count, which is precisely the case refcounting
alone is supposed to handle.
