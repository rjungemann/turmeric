# A CPS-lowered `await` never frees its future or its DK continuation frames

**Severity: low.** A fixed cost per `(async ...)` / `(await ...)` pair, not a
per-iteration growth in any loop we ship -- but a program that spawns tasks in
a loop leaks proportionally to the number of tasks. Nothing reads the leaked
memory, so there is no wrong answer here, only unreclaimed bytes.

**Status:** OPEN. Filed 2026-09-17 while landing fix direction 1 of
[compiled-async-fiber-deadlocks-on-a-session-op](../archive/compiled-async-fiber-deadlocks-on-a-session-op.md),
which added two leak-checked session/async fixtures and surfaced this. It is
**not** caused by that change: the leak reproduces identically on
`tests/fixtures/async-await-cps`, an untouched fixture with no session
involvement.

## Repro

`tests/fixtures/async-await-cps/input.tur` -- a plain `async` + `await` with no
sessions and no threads:

```turmeric
(defn compute [] : int
  ```c
  return (int64_t)42;
  ```)

(defn get-it [] : int
  (let [fut (async compute)]
    (+ 1 (await fut))))

(defn main [] : nil
  (println (get-it)))
```

```sh
$ touch tests/fixtures/async-await-cps/requires.leak-check
$ bash tests/run-leak-check.sh 2>&1 | grep -A8 'FAIL async-await-cps'
FAIL async-await-cps -- SUMMARY: AddressSanitizer: 296 byte(s) leaked in 3 allocation(s).
    Direct leak of 120 byte(s) in 1 object(s) allocated from:
        #1 ... in dk_new
    Indirect leak of 120 byte(s) in 1 object(s) allocated from:
    Indirect leak of 56 byte(s) in 1 object(s) allocated from:
        #1 ... in tur_future_new
        #2 ... in tur_async_fiber
        #3 ... in get_hyit__cps
```

Measured against `./build/tur` at v0.49.0 (2026-09-17).

## Root cause

Two owners, neither of which ever releases:

- **The future.** `tur_async_fiber` / `tur_async_fiber_closure` /
  `tur_async_fiber_via` / `tur_async_thread_via` each `tur_future_new()` a
  `TurFuture` and hand it back as the task handle. `tur_future_free` exists in
  the emitted preamble (`emit_module.c`) but **nothing calls it** -- `await`
  reads the value and walks away, and the `let`-bound handle is a bare
  `ptr<void>` with no drop glue.
- **The DK continuation frames.** On the CPS path `await` lowers to
  `dk_shift(DK_ROOT_TAG, __tur_await_body, ...)`; `__tur_await_body` resumes via
  `dk_invoke(subk, ...)` on the ready-future fast path, and the frames
  `dk_new`'d for the shift are not reaped on that path. `__dk_reap_closure`
  handles the closure box, not the frames.

The `tur_future_free` on the thread-backed path added by the deadlock fix joins
the task before freeing, so a future that IS freed is safe -- the gap is purely
that no caller frees one.

## Scope

Affects every backend arm equally (the inline spawns and the thread-backed
spawn alike), because the unfreed allocations are the future and the DK frames,
not anything the spawn flavour owns. `tests/fixtures/session-async-recv` and
`session-async-peer` carry `requires.leak-check` plus a `known-leak` marker
pointing here, so the gate stays honest and turns red on those two the moment
this is fixed and the markers come off.

## Fix directions

1. **Free the future at the await that consumes it** -- the natural owner, but
   only correct when the handle is single-consumer. A future awaited twice, or
   awaited after being stored in a container, would be a use-after-free. Needs
   the handle to stop being a bare `ptr<void>` first (it is exactly the
   `:int`/`ptr<void>` stand-in CLAUDE.md warns about): a real `Future<T>` opaque
   with drop glue would make the ownership checkable instead of assumed.
2. **Reap the shift's DK frames on the inline-resume path** of
   `__tur_await_body`, where the continuation is invoked and finished in the
   same call and nothing can resume it again. Independent of 1 and the smaller
   half.

## See also

- `src/compiler/emit_module.c` -- `tur_future_new` / `tur_future_free` /
  `__tur_await_body`, and the four spawns.
- `tests/run-leak-check.sh` -- the `requires.leak-check` / `known-leak` markers.
