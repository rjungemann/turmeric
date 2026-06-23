# STM Fine-Grained Locking Plan

## Context

The user-facing guides (`stm-guide.md`, `stm-tutorial.md`) describe the STM
runtime as "single global mutex; per-TVar locking is a future direction."
The runtime in `src/runtime/stm.c` is already past that: commit acquires
**per-TVar `pthread_mutex_t` locks** on the address-sorted write set, then
validates, applies writes, broadcasts, and unlocks in reverse order. The
`STM_State.global_lock` field declared in `stm.h` is initialized but never
acquired. Lock buckets (`STM_LockBucket lock_buckets[64]`) exist but are
only used as a per-TVar registry, not for striping.

So the real work is not "introduce per-TVar locks" -- it's **finish the
half-built fine-grained scheme already in the tree** and remove the dead
global-lock scaffolding, so the runtime matches what we want to claim in
the guides.

This plan covers v1 only. Lock-free / wait-free variants stay out of scope.

## Goals

1. Correct, contention-friendly commit + retry that scales with TVar count,
   not transaction count.
2. Remove dead scaffolding (`global_lock`, unused bucket lock fields) so the
   header reflects the chosen design.
3. Keep the existing public C API (`tur_tvar_*`, `tur_stm_commit`,
   `tur_atomically`, ...) untouched -- all changes are internal.

## Current gaps (what's actually broken or stubby)

Pulled from `src/runtime/stm.c`:

- **G1 -- Validate without read-side locks.** `tur_stm_validate` (line 218)
  reads `tv->version` while only the write-set locks are held. A concurrent
  committer touching a TVar that we *only read* can write `tv->value` and
  bump `tv->version` between our load and our compare, so we can either
  miss a conflict (if the writer's commit completes between our read and
  our version load) or read a torn value during the user phase. The fix is
  the standard TL2 / TinySTM pattern: a global version clock plus
  per-TVar version stamps and a read-snapshot timestamp.
- **G2 -- `retry()` is a stub.** Lines 288-311 only block on the *first*
  TVar in the read set, and there is no per-TVar waiter list. A retrying
  transaction misses wake-ups for any other TVar it read, so it can sleep
  forever even when a different read TVar it depends on changes.
- **G3 -- No waiter list on TVar.** The header comment at line 14 promises
  one; the struct doesn't have one. Retries currently rely on
  `pthread_cond_broadcast` on the *write* TVar's cond, which the retrier
  may not be parked on.
- **G4 -- Lock striping is declared but unused.** `lock_bucket` and
  `next_in_bucket` exist on every `TVar`, and `STM_LockBucket.lock` is
  initialized, but commit takes per-TVar locks directly. Either use the
  buckets as the actual lock granularity or delete the fields.
- **G5 -- Dead `global_lock`.** `STM_State.global_lock` is initialized,
  never locked, and contradicts the per-TVar story. Remove.
- **G6 -- Nested `atomically`.** `STM_Transaction.parent` is declared but
  never threaded through `tur_atomically`. The guide already documents
  "no nested transactions" as a limitation, so the right move is to
  delete the field, not implement nesting in this plan.

## Design

Adopt the **TL2** (Transactional Locking II) discipline, which is the
minimum scheme that makes the existing per-TVar-lock commit path correct
without serializing readers. It is well-understood, small, and a strict
generalization of what we already have.

### Global version clock

```c
/* in stm.h */
typedef struct STM_State {
    _Atomic uint64_t version_clock;     /* monotonic; bumped on commit */
    STM_LockBucket lock_buckets[STM_NUM_LOCK_BUCKETS];
} STM_State;
```

- `STM_LockBucket` becomes the **actual** lock granularity (resolves G4).
  Each TVar resolves to `lock_buckets[hash(tv) & (N-1)].lock` via a small
  inline helper `stm_bucket_for(tv)`. Per-TVar `pthread_mutex_t` and
  `next_in_bucket` come off `TVar`.
- `version_clock` is the source of truth for read-set validation.

### Transaction lifecycle

1. **Begin.** `tx->read_stamp = atomic_load(&state->version_clock)`.
2. **Read** of `tv`:
   - Snapshot `v1 = tv->version` (atomic load).
   - If `v1 > tx->read_stamp` or `v1` is locked (low bit set; see below):
     abort immediately.
   - Load `tv->value`.
   - Re-check `v2 = tv->version`; if `v1 != v2` or locked, abort.
   - Record `(tv, v1)` in the read set.
3. **Write** of `tv`: buffer `(tv, new_value)` in the write set; do not
   touch `tv->version`. (Unchanged from today, with the version field now
   atomic.)
4. **Commit** (replaces today's body of `tur_stm_commit`):
   1. Lock every bucket covering the write set, sorted by bucket index
      (lock ordering -> no deadlock).
   2. `wv = atomic_fetch_add(&state->version_clock, 1) + 1`.
   3. Re-validate the read set: each `(tv, v1)` must still satisfy
      `tv->version == v1` **unless** the TVar is in our own write set.
      Failure -> unlock, return false (caller retries the closure).
   4. Apply writes; for each written TVar set `tv->version = wv`
      (atomic store-release) and `tv->value = new`.
   5. Signal waiters on each written TVar's bucket cond (see retry).
   6. Unlock buckets in reverse order.

Locking the version field, TL2-style ("low bit = locked"), is **not**
needed because we serialize through the bucket mutex. We just need the
version load to be atomic so steps 2 and 4 race cleanly.

### retry()

Resolve G2 + G3 with a **bucket-level** waiter list, not per-TVar:

```c
typedef struct STM_LockBucket {
    pthread_mutex_t lock;
    pthread_cond_t  cond;             /* broadcast on any commit touching this bucket */
    uint64_t        commit_seq;       /* bumped every commit in this bucket */
} STM_LockBucket;
```

`retry(tx)` walks the read set, collects the *set of distinct buckets*
touched, snapshots each bucket's `commit_seq`, then sleeps on each bucket's
cond until *any* of those `commit_seq` values advances. (Standard
"sleep on any of N condvars" idiom: launch one helper thread, or
serialize -- the latter is fine for v1 given STM is not on the hot path
and `STM_MAX_READ_SET = 256` caps the distinct bucket count at 64.)

A spurious wake is harmless: the retrier just re-runs the transaction
closure, which either commits or re-parks.

This is strictly better than today's "block on first TVar" stub, and it
avoids growing TVar-side waiter lists.

### What `TVar` looks like after

```c
typedef struct TVar {
    TypeInfo        *type;
    void            *value;
    _Atomic uint64_t version;
} TVar;
```

Three fields gone (`lock`, `cond`, `lock_bucket`, `next_in_bucket`),
one made atomic. Per-TVar memory drops from ~120 bytes (mutex + cond on
glibc) to 24 bytes.

## Work items

| # | Item | File(s) | Notes |
|---|------|---------|-------|
| F1 | Add `version_clock` to `STM_State`; init in `tur_stm_init`. | `src/runtime/stm.{c,h}` | `_Atomic uint64_t`, start at 0. |
| F2 | Add `cond` + `commit_seq` to `STM_LockBucket`. | `stm.{c,h}` | Init/destroy alongside `lock`. |
| F3 | Strip `lock`, `cond`, `lock_bucket`, `next_in_bucket` from `TVar`. | `stm.{c,h}`, `tur_tvar_new`, `tur_tvar_free` | Wipes G4. |
| F4 | Delete `STM_State.global_lock` and all references. | `stm.{c,h}` | Wipes G5. |
| F5 | Delete `STM_Transaction.parent`. | `stm.h` | Wipes G6; doc already says no nesting. |
| F6 | Make `tv->version` atomic; update every read/write site. | `stm.c`, any direct `tv->version` consumer | Use `atomic_load_explicit(memory_order_acquire)` on read, `release` on write. |
| F7 | Implement TL2 read path in `tur_tvar_read`. | `stm.c` | Adds `read_stamp` to `STM_Transaction` (set in `tur_stm_new_transaction`). |
| F8 | Rewrite `tur_stm_commit` per the four-step protocol above. | `stm.c` | Replaces lines 230-272. |
| F9 | Rewrite `tur_stm_retry` to use bucket-cond waiting on the distinct buckets touched by the read set. | `stm.c` | Replaces lines 288-311. |
| F10 | Bump `commit_seq` and `cond_broadcast` per touched bucket inside the locked region of commit. | `stm.c` | Pair with F9. |
| F11 | Update `stm-guide.md` Limitations section: drop the global-lock bullet, replace with the real list (no nesting, no in-transaction I/O, fixed-size read/write/defer sets). | `docs/guides/stm-guide.md` | Mirror in `stm-tutorial.md`. |
| F12 | Delete stale "Phase 20 / Phase 21" comments in `stm.h`, replace with a one-paragraph note describing TL2. | `src/runtime/stm.h` | Mirror in `stm.c` header comment. |

F1-F5 are mechanical and independent; do them first to give F6-F10 a clean
slate. F11-F12 land in the same PR as F6-F10 so the docs and header don't
lie about the runtime.

## Testing

Existing fixtures under `tests/fixtures/stm-*` are the baseline. Add:

- **TL2 read invalidation.** Two transactions T1 (reads `a`, sleeps,
  writes `b`) and T2 (writes `a` and commits during T1's sleep). T1
  must abort and retry; assert it eventually commits with the *new*
  value of `a` visible.
- **Retry wakes across buckets.** A blocked retrier on TVars in buckets
  X and Y must be woken by a commit that touches either.
- **Lock ordering.** Two committers whose write sets are reverse-ordered
  pairs of TVars must not deadlock under stress (loop ~10^5 iterations).
- **`requires.tsan` variant.** Mark the multi-threaded fixtures so the
  TSan run exercises them; the existing read/write-set fast paths should
  be race-free under TSan once F6 is in.

All three new fixtures are interpreter-friendly and should run under both
`bash tests/run.sh` and the TSan suite without `requires.dedicated-runner`.

## Out of scope

- Nested `atomically` (kept as a documented limitation).
- Lock-free or wait-free variants.
- Adaptive bucket count (`STM_NUM_LOCK_BUCKETS` stays at 64).
- Per-thread transaction object pooling.
- STM-aware deadlock detection beyond address-sorted lock ordering.

## Risk

The TL2 read path is the only behavioral change visible to user code, and
it strictly tightens isolation -- a transaction that committed under the
current code only because validation raced through the gap will now abort
and retry, which is the correct outcome. The retry-loop in
`tur_atomically` already handles this, so no caller-visible surface
changes.
