# `rc_free_queue_drain` is O(n^2) in the number of queued blocks

**Severity:** medium -- a performance cliff, not a correctness bug. Only bites
when many blocks are pending free at once; a program that drains often keeps
the queue short and never notices.

Found while measuring CG5's payload zeroing for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../upcoming/v1/gc-cycle-collection-followup-plan.md)
(CG8 item 2) -- the drain dominated the measurement so completely that the
memset under test was invisible until the queue was taken out of the loop.

## Root cause

The queue is a FIFO implemented by shifting the whole array down one slot on
every pop. `src/runtime/rc_free_queue.c:73`:

```c
while (rc_free_queue.count > 0) {
    RcControlBlock *cb = rc_free_queue.items[0];
    memmove(rc_free_queue.items, rc_free_queue.items + 1,
            (rc_free_queue.count - 1) * sizeof(RcControlBlock *));
    rc_free_queue.count--;
    rc_cb_free(cb);
    freed++;
}
```

Each pop memmoves the remaining `count - 1` pointers, so draining `n` blocks
moves ~`n^2 / 2` pointers. At the 65536 capacity where `rc_free_queue_push`
force-drains, that is ~2.1 billion pointer-moves per drain.

The emitted replica has the identical shape --
`src/compiler/emit_module.c:9522-9526` -- so both copies are affected.

## Measurement

2M scalar alloc/drop pairs under `GC_AUTO`, which fills and force-drains the
queue ~30 times:

| | ns per alloc/drop pair |
|---|---|
| with the drain in the loop | **~4650 ns** |
| same workload, blocks not dropped (no queue) | ~130 ns |

The ~35x gap is essentially all memmove.

## Fix directions

Keep the FIFO order (nested drops append to the back and must be freed after
the current batch) but stop shifting -- track a head cursor:

```c
uint32_t head = 0;
while (head < rc_free_queue.count) {
    RcControlBlock *cb = rc_free_queue.items[head++];
    rc_cb_free(cb);       /* may push more onto the back */
}
rc_free_queue.count = 0;
```

Care needed on two points: `rc_cb_free` can push during the loop, so `count`
must be re-read each iteration (as above) rather than cached; and the array can
be reallocated by a nested push, so the element must be read through
`rc_free_queue.items` freshly each time, not through a cached base pointer.

Both copies need the same change, and `tools/gc-copy-diff.py` should stay quiet
afterwards.

## Related

`rc_free_queue_push`'s "queue full" force-drain prints to stderr on every
overflow (`rc_free_queue.c:52`), which also makes a heavy-churn program noisy.
Worth reconsidering alongside the above -- once the drain is O(n) the overflow
path is unremarkable and does not need a diagnostic.
