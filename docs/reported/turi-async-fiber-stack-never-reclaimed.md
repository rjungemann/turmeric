# Async fiber stacks are never reclaimed until env teardown (O(N) growth)

Severity: low-medium (interpreter is process-lifetime, so this is bounded
growth, not an unbounded malloc leak; but RSS/VM grows linearly with the number
of `async` fibers spawned and is only released when the env is torn down --
matters for a long-running interpreter session or a deep/wide async workload,
not for `tur build`)

## Summary

Every `(async (fn ...))` mmaps a fresh `TURI_ASYNC_STACK_SIZE` (512 KB) fiber
stack. When the fiber finishes (`TURI_FIBER_DONE`), its stack is **not**
munmap'd -- it is only reclaimed in the teardown walk at env free. So a program
that spawns N async fibers over its lifetime holds N × 512 KB of mmap
reservations (plus the faulted-in resident pages) until the interpreter exits,
even though all but a bounded set of fibers are long since done.

This is not the same defect as the (now-archived) "deeply recursive async
returns garbage" report -- values are correct. This is purely the fiber-stack
memory footprint growing with the number of performs of `async`, not with the
number *live* at once.

## Minimal repro

```turmeric
(defn a-rec [n :int] :int
  (if (= n 0) 0 (+ 1 (await (async (fn [] : int (a-rec (- n 1))))))))
(defn main [] : int (println (a-rec 8000)) 0)
```

Peak `VmRSS` scales with depth (each fiber's stack stays mapped for the whole
run; only the touched pages are resident, so ~22 KB resident per fiber here):

```
depth  500 -> peak VmRSS ~95 MB
depth 2000 -> peak VmRSS ~128 MB
depth 8000 -> peak VmRSS ~259 MB
```

The full 512 KB *virtual* reservation per fiber also accumulates, so a wide
enough async fan-out can hit `vm.max_map_count` (one mmap region per fiber) or
exhaust address space well before RSS becomes the limit.

## Root cause

- Stack is mmap'd per fiber in `EX_ASYNC` and tracked for teardown-time
  reclaim: `src/turi/eval.c:7448` (mmap) and `src/turi/eval.c:7493`
  (`turi_env_track_coro_stack`).
- The fiber is marked `TURI_FIBER_DONE` in `async_fiber_thunk`
  (`src/turi/eval.c:287`) but nothing frees `fiber->stack` there or when the
  scheduler observes the DONE fiber.
- The only release site is the teardown walk in `turi_env_free`'s helper
  (`src/turi/fiber.c:881-893`), which munmaps every tracked coro stack at env
  destruction.

So `coro_stacks` is an append-only list for the life of the env; a DONE
fiber's stack is dead but stays mapped.

## Fix directions

- Reclaim a fiber's stack when it reaches `TURI_FIBER_DONE` -- but carefully:
  the scheduler swaps *out of* the fiber's own stack in `async_fiber_thunk`
  (`swapcontext(&fiber->ctx, &env->sched_ctx)`), so the stack is still in use
  at the moment DONE is set. The munmap must happen from the scheduler side
  *after* control has returned to `sched_ctx` (e.g. in the `turi_await_future`
  / `turi_run_event_loop` loop, right after `swapcontext` returns for a fiber
  whose `state == TURI_FIBER_DONE`), never from within the thunk itself.
- Because the stack is registered in `env->coro_stacks` for teardown, freeing
  it early also has to unlink (or tombstone) the matching `TuriCoroStack` node
  so the teardown walk does not double-munmap. Simplest: give the fiber a
  back-pointer to its `TuriCoroStack` node and clear `node->base` (the walk
  already skips `base == NULL`) when reclaiming early.
- Alternatively, pool/free-list the fiber stacks: keep a small LIFO of returned
  512 KB stacks and hand them back out to the next `async`, capping live+cached
  stacks instead of munmapping on every completion (avoids mmap/munmap churn in
  tight async loops). This bounds the footprint to the peak concurrency rather
  than the cumulative spawn count.
