# Deeply recursive `async`/`await` returns a garbage (pointer) value

Severity: medium (wrong result, silent -- no crash, no error)

## Summary

A recursion that threads through `async`/`await` returns the correct value at
small depth but a garbage value (a raw heap pointer, e.g. `91328185248752` =
`0x531...`) once the depth exceeds roughly 100-500.  No guard fires, no
SIGSEGV -- `await` just yields a non-int value.

## Minimal repro (interpreter)

```turmeric
(defn a-rec [n :int] :int
  (if (= n 0) 0 (+ 1 (await (async (fn [] : int (a-rec (- n 1))))))))
(defn main [] : int (println (a-rec 500)) 0)
```

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret repro.tur
91328185248752      # expected 500
```

Measured: `a-rec 3/5/10/100` -> correct (3/5/10/100); `a-rec 500/1000/2000/5000`
-> the same garbage pointer.  The threshold sits between 100 and 500.

## Notes

- Each `(async (fn ...))` spawns a fiber with its own `mmap`'d stack
  (`TURI_ASYNC_STACK_SIZE`) and `await` suspends to the scheduler, so a deep
  chain leaves ~N fibers suspended simultaneously, each awaiting the next.  The
  garbage-pointer result suggests the future's value slot is read before/after
  it is settled (or a fiber-stack / scheduler-depth corruption), not a C-stack
  overflow (it does not crash).
- This is **not** a heap-bounding trip: it does not hit the `eval_depth` guard,
  so it does not block retiring the guard (Phase C4).  It is a distinct
  correctness defect in the fiber scheduler / future value propagation.
- Pre-existing: `EX_ASYNC` / `EX_AWAIT` and the scheduler are untouched by
  turi-c-scoped-forms-heap-bounding C1-C3 (which only changed `catch-unwind`,
  `atomically`, `ws_capturable`, and the `resume` value-arg path).  Found while
  measuring C3.

## Fix directions

Instrument the future settle/read path (`turi_future_resolve` /
`turi_future_val` / the `EX_AWAIT` resumed branch) at depth ~200 to find where a
non-int value enters `await`'s return.  Check the fiber stack size and whether
deep suspended-fiber chains corrupt `f->result` or read it while
`TURI_FUTURE_PENDING`.
