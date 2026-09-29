# `#lang r7rs`: the dynamic environment is shared by every thread

**RESOLVED 2026-09-29.** The handler stack, the wind stack, `parameterize`'s
bindings and a re-entry's delivered value live in one thread-local of the
emitted runtime, `tur_r7rs_dyn`, which a fiber carries with it from yield to
yield and from thread to thread. A parameter's cell keeps only its global
value. See *Fix* at the end.

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

## Fix (2026-09-29)

Not `^thread-local`, which the report suggested first: a stdlib
`^thread-local` makes the prelude split decline (`emit_split_refuse`, one
unit and ~3 s a build again), and it would still be the thread's, not the
fiber's. A fiber that yields inside a `guard` and resumes on another worker
thread would find that thread's handlers, and two fibers taking turns on one
thread would share them. So the state goes where the DK runtime's per-thread
state went:

- **One runtime thread-local.** `tur_r7rs_dyn` (src/compiler/emit_dk_runtime.c,
  next to the live-escape set) is four tagged words: the wind stack, the
  handler stack, the parameter bindings, a re-entry's delivered value. It is
  `TUR_THREAD_LOCAL`, read through `TUR_TLS_FRESH` under clang and MinGW gcc
  (fiber-tls-address-reuse), a host slot in `src/runtime/tur_tls.c` under
  `tur jit`, and a collector root (`r7gc_note_tls_root`). A slot never written
  is all zero, which the prelude reads as the empty list, so a new thread
  starts with no handlers, no wind frames and no parameter bindings.
- **Per fiber.** `FiberBlock` has an `r7dyn` field, and
  `tur_fiber_block_resume` swaps the thread's `tur_r7rs_dyn` for the fiber's
  and back, as it does the escapes. `calloc` makes a new fiber's empty.
- **The prelude** (stdlib/r7rs/prelude.tur) reads and writes the slots through
  three inline-C accessors, `r7rs-dyn-ref__`, `r7rs-dyn-set__` (which notes the
  word: the region store-hook rule) and `r7rs-dyn-unset?__`; the interpreter
  has natives for them (one set of slots: turi has no user-reachable thread).
  The three `def ^mut` globals are gone.
- **Parameters.** `parameterize` no longer writes the cell. It puts
  `(cell . value)` bindings (`R7rsPBind`) in front of the running code's
  binding list for the extent, as wind frames; `(p)` reads the innermost
  binding of its cell, else the cell's global value. A new thread or fiber
  sees the global values. (SRFI 18 would convey the creator's parameters to a
  thread it spawns; a raw `pthread_create` has no hook for that, so it does
  not.)
- **An inline-C body may return `any`.** Declared `: any`, it was boxed as
  the fiat `nil` value type every inline-C body has and pasted into an
  expression, which did not compile. `elab_defn` now leaves an inline-C body
  alone there, as it does for every other declared return: it returns a
  `tur_tagged_t` itself.

Measured on a 4-core Linux box. The repro above, as
`tests/fixtures/r7rs-threads-dynamic-env` (two `guard`s, a `parameterize`, a
read of the parameter outside it and a `dynamic-wind` left by an escape, per
step), and a fiber twin, `tests/fixtures/r7rs-threads-fiber-dynamic-env` (six
fibers on a three-thread `tur_scheduler_mt`, each inside its own
`parameterize`, yielding inside a `guard`, a handler and the `parameterize`):

| fixture | before | after |
| --- | --- | --- |
| `r7rs-threads-dynamic-env` | 5 of 5 runs wrong (the three failures above) | 8 of 8 right with gcc, 6 of 6 with clang, 3 of 3 under `TUR_GC_TORTURE=31`, right under `tur jit` |
| `r7rs-threads-fiber-dynamic-env` | 5 of 5 runs wrong | 8 of 8 right with gcc, 6 of 6 with clang, 3 of 3 under `TUR_GC_TORTURE=31`, right under `tur jit` |

Both run in `run-r7rs-gc.sh` (`threads-dynenv`, `threads-fiber-dynenv`) under
a collection every 31st allocation. A single-threaded loop of the same step,
400,000 times: 1.69 s before and 1.66 s after with gcc; 1.87 s and 1.97 s with
clang, where every access goes through the `TUR_TLS_FRESH` accessor.
