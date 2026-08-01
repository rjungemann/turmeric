# `rc_free_queue_drain` is O(n^2) in the number of queued blocks

**Status: FIXED 2026-07-26.** And it was not only a performance cliff -- the
same seam carried a correctness bug that made the queue fail at the one job it
exists for. See *Resolution* at the bottom. Pinned by
`tests/fixtures/rc-free-queue-deep-cascade`.

**Severity:** medium -- a performance cliff, not a correctness bug. Only bites
when many blocks are pending free at once; a program that drains often keeps
the queue short and never notices.

*(That severity assessment was wrong -- see the Resolution.)*

Found while measuring CG5's payload zeroing for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../../upcoming/v1/gc-cycle-collection-followup-plan.md)
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

---

## Resolution (2026-07-26)

The fix is the head cursor sketched above, but implementing it surfaced a
**correctness bug on the same seam that the quadratic was hiding**, plus a
behavioural divergence between the two copies. All three are closed.

### 1. The quadratic (as reported)

Popping from the front with a memmove per pop is now a `head` cursor. The
drained prefix `[0, head)` is reclaimed by compaction only when the queue would
otherwise overflow -- so the one remaining memmove runs on overflow, not per
block, and the moved-pointer total is linear in blocks freed.

A single `rc_free_queue_drain()`, timed directly:

| queued blocks | before | after |
|---|---|---|
| 4,000 | 0.61 ms | 0.04 ms |
| 16,000 | 20.8 ms | 0.24 ms |
| 65,000 | **378.2 ms** | **0.91 ms** |

End to end, 2M alloc/drop pairs: **11,563 ms -> 70 ms** (5782 -> 35 ns/pair).

### 2. The queue did not actually bound stack depth

`rc_free_queue.h` opens by stating the queue's whole purpose: drops are queued
"to maintain constant stack depth regardless of nesting depth." It did not.

The struct-field drop glue this compiler emits calls `rc_free_queue_drain()`
itself (`emit_module.c`), and that glue runs from **inside** the drain. The
nested call simply carried on freeing, so a cascade recursed one C stack frame
per link after all. Measured on a 200,000-deep chain, **on the unmodified
pre-fix code**:

```
==8569==ERROR: AddressSanitizer: stack-overflow
SUMMARY: AddressSanitizer: stack-overflow in memmove
```

-- a stack overflow in precisely the case the queue was built to prevent. The
quadratic masked it: the memmove was slow enough that nobody drove a cascade
deep enough to hit it.

A `draining` flag now makes a nested `rc_free_queue_drain()` a no-op returning
0; the outer walk owns the queue and frees the children the glue pushes, in the
same pass, still FIFO. The same 200k-deep cascade now completes clean under
ASan in both copies, and the fixture does it in 15 ms.

This guard is load-bearing rather than defensive -- removing it reproduces the
stack overflow immediately.

### 3. Overflow behaviour, which the two copies disagreed on

| | before | after |
|---|---|---|
| runtime | force-drain (reentrantly!), then *skip the block* -- a silent leak | compact -> drain if not already draining -> `realloc` to grow |
| emitted | `abort()` | compact -> drain if not already draining -> abort only as a last resort (fixed array cannot grow) |

The runtime's "cannot queue block, skipping" path leaked outright; growth is
what the GC's own vectors settled on in CG0 for exactly this situation. The
stderr chatter on every ordinary overflow is gone -- the runtime now prints only
on genuine OOM, where it leaks one block rather than aborting a process that is
already out of memory.

Note the drain-from-push is now guarded by `!draining`. That is the same shape
DEDUP-4a removed from the collector's suspect path: a free must not re-enter the
freer.

### Scope

Both copies changed, so `tools/gc-copy-diff.py` stays at 27 divergent (no new
drift) and the DEDUP-4a parity battery still passes. The two copies keep their
deliberately different *spellings* of the queue (the emitted copy is a fixed
array, the runtime a heap-grown struct -- see the note at `emit_module.c`
`rt_global_from_archive`); only the algorithm is unified.
