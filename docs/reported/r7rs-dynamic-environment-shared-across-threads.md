# `#lang r7rs`: the dynamic environment is shared by every thread

**Severity:** medium-high for threaded Scheme programs. A `guard` on one
thread catches, or misses, a `raise` on another. `dynamic-wind`'s `after`
thunks run on the wrong thread, and a `parameterize` on one thread changes the
value every other thread reads. Single-threaded programs are unaffected.

Filed 2026-09-28, found while fixing
[dk-reap-list-shared-across-threads](../archive/dk-reap-list-shared-across-threads.md).

## Repro

Four worker threads and the main thread each run this loop. The threads are
raw pthreads started from a `(turmeric ...)` module, as the
`r7rs-threads-*` fixtures do. Every step is worth 3.

```scheme
(define (step i)
  (+ (guard (e (#t 0)) (if (= i -1) (raise 'never) 1))
     (guard (e ((symbol? e) 1)) (raise 'boom))
     1))
(define (spin n)
  (let lp ((i n) (acc 0))
    (if (= i 0) acc (lp (- i 1) (+ acc (step i))))))
```

Expected `(60000 240000)` (main's 20,000 steps, and the four workers'
80,000). Every run fails, 10 out of 10, even with the DK runtime per-thread
(the fix for dk-reap-list-shared-across-threads). The failures are `tur: continuation invoked after its call/cc prompt
returned` (abort), `uncaught exception: boom` (exit 70), or `cdr: not a
pair`.

The same loop with `call/cc` escapes instead of `guard`
(`tests/fixtures/r7rs-threads-cps-entries`) passes. So what is left is the
prelude's own state, not the CPS machinery.

## Root cause

The R7RS dynamic environment lives in process-global mutable definitions in
`stdlib/r7rs/prelude.tur`:

- `r7rs-handlers__` (line 2216), the exception-handler stack. Thread A's
  `with-exception-handler` pushes onto it while thread B's `raise` reads its
  top. B then calls A's handler, and A's `guard` escape is invoked on B,
  where it is not a live escape, hence "invoked after its call/cc prompt
  returned". Or a pop on A removes B's handler, and B's `raise` finds none:
  "uncaught exception".
- `r7rs-winders__` (line 1825), the `dynamic-wind` stack. `r7rs-rewind-to__`
  pops frames until it reaches the stack it saved. With another thread's
  frames interleaved, it runs that thread's `after` thunks, or walks off the
  end.
- `r7rs-cont-delivered__` (line 2135), the value handed to a re-entered
  continuation.
- A parameter object's cell, `R7rsParam` (line 2314). `parameterize` sets
  `.value` in place and restores it on exit, so every thread sees the value
  while any thread is inside the `parameterize`.

## Fix directions

- Make the first three thread-local. The emitted runtime already has
  per-thread globals, the `^thread-local` block kept under a pthread key, and
  the collector already treats those values as roots (r7rs-gc stage C). If
  `(def ^thread-local ^mut ...)` is accepted in the prelude, that is a
  three-line change.
- A new thread should start with an empty handler and wind stack. R7RS says
  nothing about threads. SRFI 18 gives a new thread the creator's dynamic
  environment for parameters, but not the creator's handlers.
- Parameters: `parameterize` would bind in a per-thread association (the
  current thread's dynamic bindings, falling back to the cell's global
  value), rather than write the shared cell. Emitted Turmeric already
  conveys dynamic bindings to a spawned thread through a pthread key; a
  Scheme parameter could ride the same mechanism.
- Fixture: the repro above, as `r7rs-threads-guard`, in `tests/run.sh` and
  under torture in `run-r7rs-gc.sh`.
