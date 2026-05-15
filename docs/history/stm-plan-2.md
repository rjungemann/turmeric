# Software Transactional Memory (Phases 20–21)

**Status:** Planned (v2). Prerequisites: Phase 19 (Algebraic effects — `stm` desugars to a transaction closure; continuation infrastructure in place), T19 (Thread primitives — `Mutex<T>`, condition variables, `Arc<T>`). See [archive/stm-plan.md](archive/stm-plan.md) for full background and design rationale.

**Key design decisions:**
- **Haskell-style API:** `TVar<T>`, `stm` block, `atomically`, `retry`, `or-else`, `check`; `TMVar`/`TChan`/`TSem` primitives.
- **Lock-based v1:** Single global `mtx_t` for simplicity; correct by construction; replaced by fine-grained per-TVar locking in v2.
- **`stm` is a special form** handled by `elab_stm` in `src/elab.c`; `TVar::read`/`TVar::write` outside an `stm` block are compile errors (TUR-E0009).
- **`defer` inside `stm`:** defers execute at transaction commit (success) or transaction abort (failure) — not at `stm` lexical exit.
- **Exception inside `stm`:** transaction aborts (writes discarded), abort-path defers fire, then exception propagates normally.
- **`TVar` naming:** `TVar` (Haskell style); `dosync` is the ergonomic shorthand macro.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **20** | STM core (v1) | `TVar<T>`, `stm`/`atomically`, `retry`/`check`/`or-else`, `TMVar`/`TChan`/`TSem`, global lock, `stdlib/stm.tur`, unit + simple concurrency tests all passing |
| **21** | Scalable STM (v2) | Per-TVar locking with lock ordering, lock stripping, performance benchmarks, TSan-clean stress tests |

---

## Prerequisites (Phase 20)

- [ ] Confirm Phase 19 algebraic effects infrastructure is stable (handler stack, `perform`/`handle` lowering, continuation runtime all complete).
  - Decision: Phase 19 v1 is complete per roadmap. STM phase 20 may begin once T19 thread primitives (`Mutex<T>`, condition variables, `Arc<T>`) land — those are the only additional prerequisites.
- [ ] Confirm POSIX condition variables (`pthread_cond_t`) are available on all supported platforms for TVar wait queues.
  - Decision: use POSIX `pthread_cond_t` consistently with the T19 thread primitives decision (no C11 `<threads.h>` on macOS). Same `TUR_THREAD_LOCAL` macro pattern applies to STM globals.
- [ ] Decide whether STM transaction context is thread-local or explicitly passed.
  - Decision: thread-local in v1. A single `__thread STM_Transaction *current_tx` pointer tracks the active transaction per-thread. No explicit passing needed for the simple v1 global-lock implementation.
- [ ] Decide `stm` form: special form in elaborator or macro?
  - Decision: special form handled in `elab_stm` in `src/elab.c`, analogous to `elab_handle`. The `dosync` shorthand is a stdlib macro.
- [ ] Decide how `defer` inside `stm` blocks interacts with transaction commit/abort.
  - Decision: defers inside `stm` blocks execute at transaction commit (success path) or transaction abort (failure path), not at `stm` lexical exit. The transaction maintains a defer stack; defers are fired by `tur_stm_commit` or `tur_stm_abort` respectively.
- [ ] Define `TVar` naming: `TVar` (Haskell style) vs. `TxVar`.
  - Decision: `TVar` — matches Haskell and is the most widely recognized naming.
- [ ] Define exception behavior inside `stm` blocks.
  - Decision: if an exception escapes an `stm` block, the transaction aborts (all writes discarded); the exception then propagates normally to the enclosing `try`/`catch`. Transaction defers fire (abort path) before the exception propagates.
- [ ] Confirm `TVar` Send/Sync marker policy.
  - Decision: `TVar<T>` is `Sync` (shared across threads via `Arc<TVar<T>>`) but not `Send` (ownership of the `TVar` itself is not moved). `Arc<TVar<T>>` is `Send + Sync` when `T` is `Send`.

---

## Phase 20 — STM Core (v1)

**Goal:** Add Haskell-style Software Transactional Memory with a global-lock implementation suitable for low-to-medium contention workloads.

**Prerequisites:**
- Phase 19 complete (algebraic effects infrastructure stable).
- T19 complete (`Mutex<T>`, condition variables, `Arc<T>`, POSIX `pthread_cond_t`).

### S1 — Runtime data structures (`src/stm.{c,h}`)
- [ ] Define `TVar` struct: `{ TypeInfo *type; void *value; uint64_t version; STM_WaitQueue waiters; }`.
- [ ] Define `STM_Transaction` struct: `{ TVar **read_set; uint64_t *read_versions; int read_count; TVar **write_set; void **new_values; int write_count; bool retry_requested; tur_frame_t *defer_stack; }`.
- [ ] Define `STM_State` global: `{ mtx_t global_lock; }`.
- [ ] Define `STM_WaitQueue` struct: `{ STM_Transaction **waiters; int count; pthread_cond_t cond; }`.
- [ ] Implement `tur_tvar_new(TypeInfo *type, void *initial_value) → TVar *`.
- [ ] Implement `tur_tvar_read(STM_Transaction *tx, TVar *tv) → void *` (records read in transaction log).
- [ ] Implement `tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value)` (records write in transaction log).
- [ ] Implement `tur_stm_validate(STM_Transaction *tx) → bool` (checks all read versions are still current).
- [ ] Implement `tur_stm_commit(STM_Transaction *tx) → bool` (applies writes, increments versions, fires commit-path defers).
- [ ] Implement `tur_stm_abort(STM_Transaction *tx)` (discards writes, fires abort-path defers).
- [ ] Implement `tur_stm_retry(STM_Transaction *tx)` (adds transaction to wait queues of all read TVars, blocks on condition variable).
- [ ] Implement `tur_stm_check(bool condition)` (calls `tur_stm_abort` if false).
- [ ] Implement `tur_atomically(stm_fn_t fn, void *env) → void *` (outer retry loop with global lock).
- [ ] Migrate `STM_State.global_lock` and thread-local `current_tx` to `TUR_THREAD_LOCAL` in the same pass that adds thread safety to effect handler chains.

### S2 — Elaborator and codegen (`src/elab.{c,h}`, `src/emit.{c,h}`)
- [ ] Implement `elab_stm` in `src/elab.c`: delimit a transaction block, type-check body, verify `TVar::read`/`TVar::write` calls are inside `stm`.
- [ ] Implement `elab_atomically`: validate that argument is an `stm` block or returns `(STM a)`.
- [ ] Implement `elab_retry`: only valid inside `stm`; lowers to `tur_stm_retry(current_tx)`.
- [ ] Implement `elab_check`: only valid inside `stm`; lowers to `tur_stm_check(cond)`.
- [ ] Implement `elab_or_else`: takes two `stm` blocks; tries the first, retries with second if first calls `retry`.
- [ ] Emit `TVar::read` calls as `tur_tvar_read(current_tx, tv)`.
- [ ] Emit `TVar::write` calls as `tur_tvar_write(current_tx, tv, value)`.
- [ ] Emit `stm` block as a closure passed to `tur_atomically`.
- [ ] Emit `TVar::modify` as inline `read → apply fn → write` within same transaction.
- [ ] Emit `TVar::swap` as inline `read → write → return old`.
- [ ] Add static check: `TVar::read`/`TVar::write` outside an `stm` block is a compile error (TUR-E0009).

### S3 — Synchronization primitives stdlib (`stdlib/stm.tur`)
- [ ] Define `TVar` opaque type and `TVar::new`, `TVar::read`, `TVar::write`, `TVar::modify`, `TVar::swap`, `TVar::cas`.
- [ ] Implement `TMVar<T>` (wraps `TVar<(option T)>`): `TMVar::new`, `TMVar::new-empty`, `TMVar::put`, `TMVar::try-put`, `TMVar::take`, `TMVar::try-take`, `TMVar::read`, `TMVar::is-empty`.
- [ ] Implement `TChan<T>` (wraps `TVar` of a cons list): `TChan::new`, `TChan::write`, `TChan::read`, `TChan::try-read`, `TChan::peek`, `TChan::try-peek`.
- [ ] Implement `TSem` (wraps `TVar<int>`): `TSem::new`, `TSem::wait`, `TSem::try-wait`, `TSem::signal`.

### S4 — Convenience macros stdlib (`stdlib/stm.tur`)
- [ ] Implement `(with-tvar [name init] & body)` macro: creates TVar, runs body in `atomically`.
- [ ] Implement `(dosync & body)` macro: shorthand for `(atomically (stm ...))`.
- [ ] Implement `(stm-when cond & body)` macro: conditional inside `stm`.
- [ ] Implement `(stm-unless cond & body)` macro.
- [ ] Implement `TVar::cas [tv old new] : bool`: compare-and-swap within a transaction.
- [ ] Implement `TVar::update [tv f & args]`: alias for `TVar::modify` with extra args.
- [ ] Implement `(atomically-batch & txs)`: run multiple `stm` closures in one transaction.

### S5 — Fixtures (`tests/fixtures/stm/`)
- [ ] Add `stm-tvar-basic.tur` — `TVar::new`, `TVar::read`, `TVar::write` in a single-threaded transaction.
- [ ] Add `stm-tvar-modify.tur` — `TVar::modify`, `TVar::swap`, `TVar::cas`.
- [ ] Add `stm-atomicity.tur` — verify writes are not visible until commit.
- [ ] Add `stm-retry.tur` — `retry` blocks and retries when a TVar changes (single-threaded: change TVar from another `atomically` call).
- [ ] Add `stm-check.tur` — `check` aborts transaction when condition is false.
- [ ] Add `stm-or-else.tur` — `or-else` falls through to second branch when first calls `retry`.
- [ ] Add `stm-tmvar.tur` — `TMVar::put`, `TMVar::take`, `TMVar::read`, `TMVar::is-empty`.
- [ ] Add `stm-tchan.tur` — `TChan::write`, `TChan::read`, `TChan::peek`.
- [ ] Add `stm-tsem.tur` — `TSem::new`, `TSem::wait`, `TSem::signal`.
- [ ] Add `stm-defer.tur` — `defer` inside `stm` fires at commit (success) or abort (failure) rather than lexical exit.
- [ ] Add `stm-exception.tur` — exception inside `stm` aborts the transaction; exception propagates normally.
- [ ] Add `stm-dosync.tur` — `dosync` macro shorthand.
- [ ] Add `stm-with-tvar.tur` — `with-tvar` macro.
- [ ] Add concurrency fixture `stm-concurrent-writes.tur` — multiple threads writing to same TVar (requires T19).
- [ ] Add concurrency fixture `stm-concurrent-transfers.tur` — concurrent bank transfers; verify no money lost or created (requires T19).
- [ ] Add stress fixture `stm-stress.tur` — high-contention increment benchmark (requires T19).
- [ ] Add integration fixture `stm-with-arc.tur` — `Arc<TVar<T>>` shared across threads.
- [ ] Add integration fixture `stm-with-threads.tur` — STM combined with `Thread` spawn/join.
- [ ] Add negative fixture `stm-read-outside-transaction.tur` — `TVar::read` outside `stm` is a compile error (TUR-E0009).
- [ ] Add negative fixture `stm-write-outside-transaction.tur` — `TVar::write` outside `stm` is a compile error.
- [ ] Add codegen snapshots: `stm` block lowers to closure + `tur_atomically`; `TVar::read`/`write` lower to `tur_tvar_read`/`tur_tvar_write`.

**Exit criterion:** STM works correctly for single-threaded and simple multi-threaded use cases; `TMVar`/`TChan`/`TSem` are available in stdlib; `defer` and exception integration is correct; all unit fixtures pass; concurrency fixtures pass under ThreadSanitizer.

---

## Phase 21 — Scalable STM (v2)

**Goal:** Replace the global lock with per-TVar fine-grained locking to support high-concurrency workloads.

**Prerequisites:** Phase 20 (Core STM).

### Fine-grained locking — `src/stm.{c,h}`
- [ ] Add `mtx_t lock` field to `TVar` struct.
- [ ] Replace `STM_State.global_lock` acquire/release with per-TVar lock acquisition during commit.
- [ ] Implement lock ordering: acquire TVar locks in address order during commit phase to prevent deadlocks.
- [ ] Implement lock stripping: group TVars into N lock buckets (default: 64) to reduce per-TVar overhead.
- [ ] Update `tur_stm_commit` to use per-TVar locks: acquire all write-set locks in order, validate read set, apply writes, release locks.
- [ ] Update `tur_stm_retry` to use per-TVar condition variables.

### Performance benchmarks — `tests/benchmarks/stm/`
- [ ] `stm-counter`: single TVar increment loop; target < 100 ns/op with fine-grained locking.
- [ ] `stm-transfer`: transfer between 2 TVars; target < 200 ns/transfer.
- [ ] `stm-bank`: concurrent bank simulation; verify linear scalability across thread counts.
- [ ] `stm-tchan-throughput`: `TChan` write/read throughput; target > 1M ops/sec.

### Stress and validation — `tests/fixtures/stm/`
- [ ] `stm-deadlock-free.tur` — complex multi-TVar transaction patterns; verify no deadlocks.
- [ ] `stm-starvation.tur` — verify fairness: no transaction is indefinitely starved.
- [ ] Re-run all Phase 20 concurrency and stress fixtures under ThreadSanitizer.

**Exit criterion:** STM scales to high-concurrency workloads with acceptable performance; lock-ordering prevents deadlocks; all fixtures pass under ThreadSanitizer with no data races.
