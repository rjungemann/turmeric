# Deferred Tasks Backlog — Phases T19, T20, and T21

Purpose: track the remaining work for Phases T19 (Thread Primitives), T20 (Thread Pool), and T21 (Fibers and Effects Integration), why items were deferred, and the concrete next steps for each.

---

## Current Status Summary

| Phase | Status | Blocking items |
|---|---|---|
| **T19** | 🔄 Partially complete | `Send`/`Sync` traits, `AsyncChan`, `Select`, stdlib promotion, stress fixtures, TSan sweep |
| **T20** | 📋 Not started | Blocked on T19 exit criterion |
| **T21** | 📋 Not started | Blocked on T19 + Phase 18 + Phase 19 stability |

### T19 — What is already done

The following T19 items have been completed (see `deferred-tasks-phase15-phase19.md` for full details):

- `src/platform.h` with `TUR_THREAD_LOCAL` macro.
- `src/arc.{c,h}` — `Arc<T>` with `__atomic_*`-based refcounting.
- `stdlib/atomic.tur` — `Atomic<T>` with all memory-ordering options.
- `pthread_mutex_t`-based mutex (fixture: `tests/fixtures/mutex-basic/`).
- `pthread_rwlock_t`-based read-write lock (fixture: `tests/fixtures/rwlock-basic/`).
- `pthread_cond_t`-based condition variable (fixture: `tests/fixtures/condvar-basic/`).
- `pthread_once_t`-based `Once` (fixture: `tests/fixtures/once-basic/`).
- `pthread_create`/`pthread_join`-based thread spawn/join (fixture: `tests/fixtures/thread-basic/`).
- `__thread`-based thread-local storage (fixture: `tests/fixtures/thread-local-basic/`).
- Synchronous bounded channel via mutex+condvar ring-buffer (fixture: `tests/fixtures/channel-basic/`).
- TLS migration: `global_handler_chain` and `global_effect_handler_chain` prefixed `TUR_THREAD_LOCAL`.
- Core fixtures: `thread-basic`, `arc-basic`, `mutex-basic`, `atomic-basic`, `rwlock-basic`, `condvar-basic`, `once-basic`, `thread-arc`, `thread-local-basic`, `channel-basic`.

### T19 — What remains

- **`Send`/`Sync` marker traits** in the type system and elaborator (largest remaining item).
- **Borrow checker integration**: reject non-`Send` types captured by thread closures.
- **`AsyncChan<T>`**: buffered async channel with `try-send`/`try-recv`.
- **`Select`**: multi-channel select with optional `default` branch.
- **Stdlib promotion**: current mutex/rwlock/condvar/once/thread/chan implementations exist only as fixture-level inline C. They need proper `stdlib/*.tur` wrapper files for use in user code.
- **Integration fixtures**: `threaded-fizzbuzz.tur`, `producer-consumer.tur`.
- **Stress fixtures**: `thread-stress.tur`, `mutex-stress.tur`, `atomic-stress.tur`.
- **Negative fixtures**: `thread-send-ref.tur`, `thread-send-cont.tur` (require `Send`/`Sync` enforcement).
- **Codegen snapshots**: thread spawn, `Arc` refcount, `Mutex` lock/unlock.
- **Test runner support**: `expected.timeout` and `TUR_TSAN=1`/`requires.tsan` infrastructure.
- **TSan sweep**: run all thread fixtures under ThreadSanitizer.

---

## Key Design Decisions Already Made

These decisions were finalised in the T19–T21 prerequisites section of
`deferred-tasks-phase15-phase19.md` and must not be re-opened:

- **Threading backend**: POSIX `<pthread.h>` on all platforms (C11 `<threads.h>` absent on macOS).
- **`TUR_THREAD_LOCAL`**: defined in `src/platform.h`; expands to `__thread` on GCC/Clang.
- **`Arc<T>` layout**: `src/arc.{c,h}`; `_Atomic uint64_t` refcount; `AtomicRcControlBlock` shape.
- **`Send`/`Sync`**: compiler-enforced marker properties, not typeclass traits; auto-derived; not user-implementable in v1.
- **`Mutex` poison**: `Result`-based (`mutex-lock` returns `(result T MutexPoisonError)`).
- **`-pthread` linking**: elaborator sets `needs_pthread` flag in `EmitCtx`; appended at link time in `src/emit.c`.
- **Multi-threaded test runner**: `expected.timeout` (default 10 s); `TUR_TSAN=1` opt-in; fixtures needing TSan include `requires.tsan`.
- **STM prerequisite**: Phase 20 may begin when T19's `Mutex<T>`, condition variables, and `Arc<T>` land — all three are already complete.
- **T21 fiber context switching**: hand-rolled assembly context switch via `tur_ctx_swap` (`src/fiber_ctx_x64.S`, `src/fiber_ctx_arm64.S`) and `tur_ctx_t` (`src/fiber_ctx.h`).

---

## Actionable Tasks

### T19-A — Test-runner infrastructure (prerequisite for stress and TSan work)

Do this first; it unblocks the TSan sweep and all timeout-sensitive fixtures.

- [ ] Add `expected.timeout` support to `tests/run.sh`.
  - Read `expected.timeout` from the fixture directory; default `10` when absent.
  - Kill the compiled binary and fail the test if it runs longer than the timeout.
  - Use `timeout(1)` (GNU coreutils) or the equivalent `perl`/`python` fallback on macOS (`gtimeout` via Homebrew coreutils, or `perl -e 'alarm N; exec @ARGV'`).
- [ ] Add `TUR_TSAN=1` build-mode support to `Makefile` and `tests/run.sh`.
  - When `TUR_TSAN=1`: pass `-fsanitize=thread -g` to all compile commands; emit the `-pthread` flag.
  - In `tests/run.sh`: skip fixtures whose directory contains a `requires.tsan` marker when `TUR_TSAN` is not set; run them normally when it is set.
- [ ] Document the `expected.timeout` and `requires.tsan` conventions in `docs/test-runner-contract.md`.

### T19-B — `Send`/`Sync` marker traits

This is the largest remaining T19 item and blocks the negative fixtures.

#### Type system (`src/types.{c,h}`)
- [ ] Add `MARKER_SEND` and `MARKER_SYNC` bit-flags (or booleans) to `Type` (or to a `TypeMarkers` struct).
- [ ] Auto-derive on construction:
  - Primitives (`int`, `bool`, `float`, `double`, `cstr`, `nil`, `uint*`, `int*`): `Send | Sync`.
  - `ptr<T>`, `ref<T>`, `rc<T>`, `cont<T>`: neither `Send` nor `Sync`.
  - `arc<T>`: `Send | Sync` iff `T` is `Send | Sync`.
  - `Mutex<T>`: `Send | Sync` iff `T` is `Send`.
  - `RwLock<T>`: `Send | Sync` iff `T` is `Send`.
  - Tuples and structs: derived iff every field satisfies the constraint.
  - `Chan<T>`, `AsyncChan<T>`: `Send | Sync` — the channel handle itself is safe to share.
- [ ] Enforce `Sync ⟹ Send` in the derivation rules (any type that is `Sync` is automatically also `Send`).

#### Elaborator (`src/elab.{c,h}`)
- [ ] Add `elab_check_send(Type *t, Span span, Diag *d)` helper: emits `TUR-E00XX` (new diagnostic code) when `!t->markers.send`.
- [ ] In `elab_thread` (the `thread` form elaboration): verify that all types captured by the thread closure satisfy `Send`. Use the borrow checker's closure capture list.

#### Borrow checker (`src/borrow_check.{c,h}`)
- [ ] After resolving the capture set of a thread-spawn closure, walk each captured binding's type and call `elab_check_send`; emit a use-site diagnostic on violation.

#### Diagnostics (`src/diag.{c,h}`)
- [ ] Add `TUR_E0012_NOT_SEND` diagnostic code (or the next available slot): "`type T cannot be sent across thread boundaries (not Send)`".
- [ ] Add `TUR_E0013_NOT_SYNC` diagnostic code: "`type T cannot be shared across thread boundaries (not Sync)`".

#### Fixtures
- [ ] `tests/fixtures/errors/thread-send-ref/` — sending `ref<T>` to a thread is a compile error (`TUR_E0012`).
- [ ] `tests/fixtures/errors/thread-send-cont/` — sending `cont<T>` to a thread is a compile error (`TUR_E0012`).

### T19-C — Stdlib wrapper files

All the following sync primitives currently exist only as fixture-level inline C. Each needs a proper `stdlib/*.tur` file so user programs can import them without copy-pasting.

- [ ] Create `stdlib/mutex.tur`.
  - Wraps `pthread_mutex_t` via `extern-c`.
  - Exports: `mutex-new`, `mutex-lock`, `mutex-unlock`, `mutex-try-lock`, `mutex-free`, `mutex-with-lock` (scoped macro using `defer`).
  - Poison variant: `mutex-lock-checked` returns `(result T MutexPoisonError)`.
- [ ] Create `stdlib/rwlock.tur`.
  - Wraps `pthread_rwlock_t`.
  - Exports: `rwlock-new`, `rwlock-rdlock`, `rwlock-wrlock`, `rwlock-try-rdlock`, `rwlock-try-wrlock`, `rwlock-unlock`, `rwlock-free`.
  - Scoped macros: `rwlock-with-read`, `rwlock-with-write` (using `defer`).
- [ ] Create `stdlib/condvar.tur`.
  - Wraps `pthread_cond_t`.
  - Exports: `condvar-new`, `condvar-wait`, `condvar-signal`, `condvar-broadcast`, `condvar-free`.
- [ ] Create `stdlib/sync.tur`.
  - Exports: `Once` / `once-flag-new`, `once-call`, `once-flag-free` (wrapping `pthread_once_t`).
  - Exports: `Semaphore` / `sem-new`, `sem-acquire`, `sem-release`, `sem-free` (Mutex + Condvar + counter; `Semaphore` moved from T20 since it fits naturally here).
  - Note: `Barrier` is deferred — `pthread_barrier_t` is absent on macOS.
- [ ] Create `stdlib/thread.tur`.
  - Exports: `thread-spawn` (wraps `pthread_create` + trampoline), `thread-join`, `thread-detach`, `thread-id`, `thread-done?`.
  - Exports: `thread-local-new`, `thread-local-get`, `thread-local-set!` (wrapping `pthread_key_t` / `tss_t` key-based TLS or `__thread` statics).
  - Thread attributes: `:stack-size`, `:detached`, `:name`.

### T19-D — `AsyncChan<T>` and `Select`

These are the two remaining channel primitives.

#### `AsyncChan<T>` — buffered async channel
- [ ] Add to `stdlib/chan.tur` (create file; promote existing `Chan<T>` implementation from fixture inline C too).
  - `AsyncChan`: mutex + condvar + dynamic ring-buffer with configurable capacity.
  - Exports: `async-chan-new`, `async-chan-send` (blocks when full), `async-chan-recv` (blocks when empty).
  - Non-blocking variants: `async-chan-try-send` (returns `:full` on a full buffer), `async-chan-try-recv` (returns `:empty`).
  - `async-chan-free`.
- [ ] Add fixture `tests/fixtures/async-channel/`.

#### `Select` — multi-channel select
- [ ] Design: each `select` branch registers itself with the channel's internal wait queue.
  - Implement as a state-machine struct allocated on the stack; branches are indexed by an integer ID.
  - A `default` branch (if present) causes `select` to return immediately when no channel is ready.
  - Lowering in `src/emit.c`: `(select (recv ch1 x) body1 (recv ch2 y) body2 :default body3)` → state-machine C code.
- [ ] Add `elab_select` in `src/elab.c`.
- [ ] Add `select` form to `src/reader.c`.
- [ ] Add fixture `tests/fixtures/select-basic/`.

### T19-E — Integration and stress fixtures

These require T19-B (`Send`/`Sync`) and T19-C (proper stdlib) to be done first.

- [ ] `tests/fixtures/threaded-fizzbuzz/` — multi-threaded FizzBuzz; one thread handles multiples of 3, another multiples of 5.
- [ ] `tests/fixtures/producer-consumer/` — producer thread sends integers over a `Chan<T>`; consumer reads and sums; verify result.
- [ ] `tests/fixtures/thread-stress/` — spawn 1000 threads, each incrementing a shared `Arc<Mutex<int>>`; verify final count. Include `requires.tsan`.
- [ ] `tests/fixtures/mutex-stress/` — 10 threads contend on a `Mutex<int>`; sum increments; verify count. Include `requires.tsan`.
- [ ] `tests/fixtures/atomic-stress/` — 100 threads each do 1000 `atomic-add!`; verify final sum = 100,000. Include `requires.tsan`.

### T19-F — Codegen snapshots

- [ ] `tests/fixtures/thread-spawn-snapshot/` — minimal `thread-spawn` + join; golden `expected.c` covers the `pthread_create` trampoline pattern.
- [ ] `tests/fixtures/arc-refcount-snapshot/` — `arc-new` + `arc-clone` + `arc-drop`; golden `expected.c` covers `__atomic_fetch_add`/`__atomic_fetch_sub`.
- [ ] `tests/fixtures/mutex-snapshot/` — `mutex-new` + `mutex-lock` + `mutex-unlock` + `mutex-free`; golden `expected.c` covers `pthread_mutex_*` calls.

### T19-G — ThreadSanitizer sweep

Do this last within T19, once all fixtures and stdlib files are stable.

- [ ] Add `requires.tsan` marker to: `thread-basic`, `thread-arc`, `thread-stress`, `mutex-stress`, `atomic-stress`, `threaded-fizzbuzz`, `producer-consumer`, `async-channel`, `select-basic`, `channel-basic`.
- [ ] Run `TUR_TSAN=1 make test` and confirm all thread fixtures pass under TSan.
- [ ] Fix any data races or TSan findings before marking T19 complete.

---

### T20 — Thread Pool and Higher-Level Abstractions

**Prerequisites:** T19 exit criterion met (`Arc<T>`, `Mutex<T>`, `Chan<T>`, `Thread`/`JoinHandle`, `Send`/`Sync` enforcement, all fixtures passing under TSan).

#### T20-A — `Semaphore`

Already noted in T19-C; `Semaphore` lives in `stdlib/sync.tur`. If it is implemented there, mark this done. If not:

- [ ] Implement `Semaphore` in `stdlib/sync.tur`: `sem-new`, `sem-acquire`, `sem-release`, `sem-free`.
  - Implementation: `Mutex` + `Condvar` + integer counter (no `pthread_sem_t` — not available on macOS in unnamed form).
- [ ] Add fixture `tests/fixtures/semaphore/`.

#### T20-B — `WorkQueue<T>`

- [ ] Implement in `stdlib/threadpool.tur`:
  - `work-queue-new` (unbounded), `work-queue-new-bounded` (max capacity).
  - `work-queue-push` (blocks on push when bounded and full), `work-queue-pop` (blocks when empty).
  - `work-queue-free`.
  - Implementation: `Mutex` + `Condvar` + dynamic array (unbounded) or ring-buffer (bounded).
- [ ] Add fixture `tests/fixtures/work-queue/` — producer/consumer with bounded queue; verify all items received.

#### T20-C — `Future<T>` and `Promise<T>`

- [ ] Implement in `stdlib/future.tur`:
  - `promise-new` → returns a `(Promise<T>, Future<T>)` pair.
  - `promise-fulfill` — set value; wake all blocked `future-get` callers.
  - `promise-fail` — set an exception value; `future-get` returns `Err`.
  - `future-get` — blocks until value is available (Condvar wait); returns `(result T exn)`.
  - `future-done?` — non-blocking; returns `bool`.
  - `future-free`, `promise-free`.
  - Implementation: shared heap cell containing `Mutex` + `Condvar` + optional value + optional exception.
- [ ] Add fixture `tests/fixtures/future-basic/` — `promise-fulfill` on one thread; `future-get` on another.
- [ ] Add fixture `tests/fixtures/future-error/` — `promise-fail` propagates error to `future-get`.

#### T20-D — `ThreadPool`

- [ ] Implement fixed-size pool in `stdlib/threadpool.tur`:
  - `thread-pool-new` — spawn N worker threads each blocking on a `WorkQueue` pop.
  - `thread-pool-submit` — push closure to queue; returns `Future<T>`.
  - `thread-pool-shutdown` — push N sentinel items (or use a separate stop flag); join all workers.
- [ ] Implement dynamic-scaling pool in `stdlib/threadpool.tur`:
  - `thread-pool-new-dynamic` — min/max thread count; spawn on demand when all threads busy.
  - Idle threads above min are cleaned up after a configurable idle timeout.
- [ ] Add fixture `tests/fixtures/thread-pool-basic/` — submit 10 tasks; collect and sum futures.
- [ ] Add fixture `tests/fixtures/thread-pool-dynamic/` — burst of tasks triggers scale-up; verify correct results.
- [ ] Add fixture `tests/fixtures/thread-pool-shutdown/` — verify `thread-pool-shutdown` drains in-flight tasks before returning.

#### T20-E — Integration fixture

- [ ] Add `tests/fixtures/raytracer/` — simple parallel ray-tracer (sphere intersection); divide scanlines across thread pool workers; verify pixel output matches serial reference.
  - This is the T20 exit criterion integration test; keep geometry trivial (1–3 spheres, small resolution like 32×32) to keep fixture output manageable.

---

### T21 — Fibers and Effects Integration (v2)

**Prerequisites:**
- T19 exit criterion met (OS threads provide the scheduler host).
- Phase 18 stable (`reset`/`shift` CPS substrate for effect handler integration).
- Phase 19 stable (algebraic effects handler chain; `global_effect_handler_chain` already TLS).

**Important:** T21 landing directly unblocks Phase 19 Section B item "Implement per-fiber handler stack representation" — currently blocked and marked as such in `deferred-tasks-phase15-phase19.md`.

#### T21-A — Fiber runtime (`src/fiber.{c,h}`)

- [ ] Define `TurFiber` struct in `src/fiber.h`:
  ```c
  typedef struct TurFiber {
      tur_ctx_t   ctx;          /* fiber execution context            */
      tur_ctx_t   caller_ctx;   /* context to return to on yield      */
      void       *stack;        /* heap-allocated stack               */
      size_t      stack_size;   /* default: 1 MiB                     */
      bool        done;         /* true after fiber closure returns   */
      void       *result;       /* return value (set when done)       */
      void       *arg;          /* argument passed on first resume    */
      tur_handler_t *handler_chain; /* fiber-local effect handler chain */
  } TurFiber;
  ```
- [ ] Implement `tur_fiber_new(void (*fn)(TurFiber *), size_t stack_size) → TurFiber *`.
  - Allocate stack via `malloc`; initialize `ctx` fields (`rip`/`rsp` on x64, `lr`/`sp` on arm64) for first entry through `fiber_entry_shim`.
- [ ] Implement `tur_fiber_resume(TurFiber *f, void *arg) → void *`.
  - Sets `f->arg`, then calls `tur_ctx_swap(&f->caller_ctx, &f->ctx)`.
  - Returns `f->result` when the fiber has yielded or completed.
- [ ] Implement `tur_fiber_yield(TurFiber *f, void *value)`.
  - Sets `f->result = value`, then calls `tur_ctx_swap(&f->ctx, &f->caller_ctx)`.
- [ ] Implement `tur_fiber_done(TurFiber *f) → bool`.
- [ ] Implement `tur_fiber_free(TurFiber *f)` — frees the stack and the struct.
- [ ] Add `src/fiber.c` and `src/fiber.h` to `Makefile` and `src/main.c` include set.

**Context-switching platform note:** Use the assembly context switch path (`tur_ctx_swap`) for x86-64 and arm64. Do not use `setjmp`/`longjmp` — they do not switch stacks and cannot back a real fiber.

#### T21-B — Fiber-local effect handler chain

- [ ] Update `tur_effect_perform` (in `src/emit.c` preamble / `src/runtime.c`) to walk `current_fiber->handler_chain` instead of the global `global_effect_handler_chain` when a fiber is active.
  - Add a `TUR_THREAD_LOCAL TurFiber *tur_current_fiber = NULL` global in `src/fiber.c`.
  - `tur_fiber_resume` sets `tur_current_fiber = f` before `swapcontext`; restores to previous value after.
- [ ] This directly implements Phase 19 Section B "Implement per-fiber handler stack representation".
- [ ] Mark that item done in `deferred-tasks-phase15-phase19.md` once T21-B lands.

#### T21-C — `stdlib/fiber.tur` — Turmeric API

- [ ] Create `stdlib/fiber.tur`:
  - `fiber-new` — wraps `tur_fiber_new`; takes a closure `(fn [arg] ...)`.
  - `fiber-resume` — wraps `tur_fiber_resume`; returns yielded value or result.
  - `fiber-yield` — wraps `tur_fiber_yield`; pauses current fiber.
  - `fiber-done?` — wraps `tur_fiber_done`.
  - `fiber-result` — returns final value after `fiber-done?` is true.
  - `fiber-free` — wraps `tur_fiber_free`.
  - Fiber-local storage: `fiber-local-new`, `fiber-local-get`, `fiber-local-set!` — key-based lookup in a per-fiber association list stored on the `TurFiber` struct.
- [ ] Add fixture `tests/fixtures/fiber-basic/` — create fiber, resume once, verify return value.
- [ ] Add fixture `tests/fixtures/fiber-yield/` — fiber yields intermediate values; caller collects them.

#### T21-D — Cooperative scheduler

- [ ] Add `Scheduler` to `stdlib/fiber.tur` (or a new `stdlib/scheduler.tur`):
  - `scheduler-new` — allocate scheduler struct (work queue of runnable fibers).
  - `scheduler-spawn` — create a fiber and enqueue it; returns a `Future<T>` backed by the fiber.
  - `scheduler-run-to-completion` — loop: pop next runnable fiber, resume it; repeat until queue empty.
  - Idle detection: if all fibers are blocked (waiting on channels), terminate with an error (no progress possible).
- [ ] Channel integration: `chan-recv` and `async-chan-recv` check if a scheduler is active; if so, yield the current fiber to the scheduler rather than blocking the OS thread.
  - Add a `TUR_THREAD_LOCAL TurScheduler *tur_current_scheduler = NULL` global.
  - Blocked channel operations set the fiber as "waiting" and yield to the scheduler.
- [ ] Add fixture `tests/fixtures/fiber-scheduler/` — three fibers cooperatively print values in round-robin order; verify output sequence.

#### T21-E — Continuation / effect integration

- [ ] Enforce that continuations captured inside a fiber cannot escape to a different fiber.
  - On `tur_cont_resume`, check that `tur_current_fiber == cont->origin_fiber` (store `origin_fiber` on `TurCont` struct at capture time).
  - If mismatch: `fprintf(stderr, "continuation error: resume on wrong fiber\n"); abort()`.
- [ ] Add negative fixture `tests/fixtures/errors/fiber-cross-resume/` — attempt cross-fiber continuation resume; expect nonzero exit and `"continuation error: resume on wrong fiber"` in stderr.

#### T21-F — `async`/`await` sugar

- [ ] Parse `(async expr)` in `src/reader.c` — new special form token.
- [ ] Parse `(await expr)` in `src/reader.c` — new special form token.
- [ ] Implement `elab_async` in `src/elab.c`:
  - Wraps `expr` in `(fiber-new (fn [_] expr))`.
  - Enqueues fiber to `tur_current_scheduler`.
  - Returns a `Future<T>` backed by the fiber's result.
- [ ] Implement `elab_await` in `src/elab.c`:
  - If `future-done?` is true: return value immediately.
  - Otherwise: yield current fiber; scheduler resumes when future fulfills.
  - Lowering: `(await fut)` → check done; if not, `fiber-yield` to scheduler with `fut` as token; scheduler resumes this fiber when `promise-fulfill` fires.
- [ ] Add `thread-pool-submit-async` in `stdlib/threadpool.tur`: submit an `async` closure to the thread pool; returns `Future<T>`.
- [ ] Add fixture `tests/fixtures/async-await-basic/` — `(async ...)` creates a fiber; `(await ...)` retrieves the result.
- [ ] Add fixture `tests/fixtures/async-await-channel/` — async producer writes to `AsyncChan`; async consumer reads; `await` collects result.

---

## Suggested Execution Order

### Within T19 (complete these before moving to T20)

1. **T19-A**: Test runner (`expected.timeout`, `TUR_TSAN=1`/`requires.tsan`) — unblocks stress fixtures.
2. **T19-B**: `Send`/`Sync` marker traits (types → elaborator → borrow checker → diagnostics) — unblocks negative fixtures.
3. **T19-C**: Stdlib promotion (`stdlib/mutex.tur`, `stdlib/rwlock.tur`, `stdlib/condvar.tur`, `stdlib/sync.tur`, `stdlib/thread.tur`) — unblocks integration fixtures and T20.
4. **T19-D**: `AsyncChan<T>` (simpler, implement first) + `Select` (more complex state machine, implement after).
   - Create `stdlib/chan.tur` consolidating `Chan<T>` and `AsyncChan<T>`.
5. **T19-E**: Integration and stress fixtures (requires T19-B and T19-C complete).
6. **T19-F**: Codegen snapshots.
7. **T19-G**: TSan sweep — final gate before declaring T19 complete.

### Within T20 (complete these before moving to T21)

1. **T20-A**: `Semaphore` (if not already in `stdlib/sync.tur` from T19-C).
2. **T20-B**: `WorkQueue<T>` — prerequisite for `ThreadPool`.
3. **T20-C**: `Future<T>` / `Promise<T>` — prerequisite for `ThreadPool::submit`.
4. **T20-D**: `ThreadPool` (fixed-size first, then dynamic).
5. **T20-E**: `raytracer.tur` integration fixture — T20 exit criterion.

### Within T21

1. **T21-A**: Fiber runtime C layer (`src/fiber.{c,h}`) — all other T21 work depends on this.
2. **T21-B**: Fiber-local effect handler chain — do immediately after T21-A; directly unblocks the Phase 19 Section B deferred item.
3. **T21-C**: `stdlib/fiber.tur` API + basic fixtures.
4. **T21-D**: Cooperative scheduler + channel integration.
5. **T21-E**: Cross-fiber continuation enforcement + negative fixture.
6. **T21-F**: `async`/`await` sugar (reader → elaborator) + `submit-async` + fixtures.

---

## Dependency Graph (Simplified)

```
T19-A (test runner)
  └→ T19-G (TSan sweep)

T19-B (Send/Sync)
  └→ negative fixtures (thread-send-ref, thread-send-cont)

T19-C (stdlib promotion)
  ├→ T19-D (AsyncChan, Select)
  ├→ T19-E (integration fixtures)
  └→ T20-B, T20-C, T20-D

T19 exit criterion
  └→ T20-A → T20-B → T20-C → T20-D → T20-E

T19 + Phase 18 + Phase 19
  └→ T21-A → T21-B → T21-C → T21-D → T21-E → T21-F
                ↓
        Phase 19 Section B unblocked
```

---

## Notes

- `Barrier` remains deferred throughout T19–T21: `pthread_barrier_t` is absent on macOS, and a `Mutex`+`Condvar`+counter fallback adds complexity for a rarely-needed primitive. Revisit when a genuine use case arises.
- `Select` (T19-D) is the most architecturally complex remaining T19 item. If it threatens the T19 timeline, it may be deferred to T20 alongside `WorkQueue` and moved after `AsyncChan` and the stdlib promotion are done.
- Phase 20 (STM core) and Phase 21 (STM scalable) from `turmeric-plan.md` both require T19 to be complete. They are not listed here because their detailed task breakdown already exists in `deferred-tasks-phase15-phase19.md` (STM prerequisites and remaining tasks sections).
- Keep this file aligned with `docs/turmeric-plan.md` when phase statuses change.
