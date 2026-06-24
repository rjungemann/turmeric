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

## Open question: tur + turi parity

Turmeric ships **two** STM implementations:

- **`tur` (compiled runtime)** -- `src/runtime/stm.{c,h}`, what this
  plan rewrites.
- **`turi` (tree-walking interpreter)** -- `src/turi/eval.c` (around
  lines 1888-1950 and 7100-7180) has its own `TuriTVar { value;
  version }` and its own commit/retry path that AOT codegen never
  touches.

Most `tests/fixtures/stm-*` exercise the `turi` path. If we land TL2
only in `tur`, the new fixtures in the Testing section below won't
actually run the rewritten code unless we also compile them (i.e. make
them `runtime`-only, not interpreter-included).

**Tentative decision: bring `turi` along to the same TL2 discipline in
the same PR**, because (a) the parity rule we've held everywhere else
(by-value type behavior, sized types, dynvars) means a user who
prototypes in `turi` and ships in `tur` must see the same isolation
semantics, and (b) `turi`'s STM is much smaller -- it doesn't have the
bucket striping or the cond-per-TVar baggage, so adding the global
version clock + read-stamp + abort flag is on the order of 100-200
lines.

If we punt `turi`, that's a real visible-semantics divergence and the
plan needs to call it out in `stm-guide.md` ("interpreted STM uses a
single global mutex; compiled STM uses TL2 -- a transaction that
aborts under TL2 may commit under the interpreter"). We should decide
before F-work starts, not after. Tracking as **F0**.

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
- **G7 -- Commit-defers never fire.** Neither `tur_stm_commit` nor
  `tur_atomically` (`stm.c:326-381`) ever calls
  `tur_stm_fire_commit_defers`. This is a pre-existing bug the rewrite
  should fix in passing: fire deferred callbacks inside the commit's
  locked region (or, if we keep punting, document it explicitly so the
  rewrite doesn't quietly entrench it).
- **G8 -- Bucket hash is degenerate.** `addr % STM_NUM_LOCK_BUCKETS`
  (`stm.c:62`) on 16-byte-aligned `malloc` results gives only ~4
  effective buckets out of 64. Fix as part of F3: use
  `(addr >> 4) & (N-1)` (or a small mix) so striping is actually
  distributing TVars.
- **G9 -- `tur_tvar_read` has no error channel.** It returns `void *`
  and callers (including codegen at `emit_expr.c:4771-4831` and the
  inline-C bodies in `elab_concurrent.c`) have no way to short-circuit
  on a mid-transaction conflict. Today this is moot because read
  conflicts only surface at commit; under TL2 reads must abort
  immediately. The rewrite needs an explicit mechanism -- e.g. set
  `tx->aborted = true`, have `tur_tvar_read` return NULL on abort, and
  have `tur_atomically`'s retry loop treat "aborted mid-body" as
  "restart from the top" rather than the current "return NULL to
  caller" behavior at `stm.c:355-358`.

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
  `next_in_bucket` come off `TVar`. The hash must shift past the
  alignment bits (e.g. `(addr >> 4)`) so striping isn't degenerate
  (resolves G8).
- `version_clock` is the source of truth for read-set validation. Loads
  use `memory_order_acquire`; the commit-time `fetch_add` is
  `memory_order_acq_rel`. (Acquire-only on the begin-time load would
  let a read be reordered past a writer whose version is `<=`
  `read_stamp`.)

### Transaction lifecycle

1. **Begin.** `tx->read_stamp = atomic_load(&state->version_clock)`.
2. **Read** of `tv`:
   - Snapshot `v1 = tv->version` (atomic load, acquire).
   - If `v1 > tx->read_stamp` or `v1` is locked (low bit set; see below):
     abort immediately (see "abort channel" below).
   - Load `tv->value` (atomic load on the `void *` slot -- on 64-bit
     targets the pointer is naturally word-atomic, but we spell it out
     so TSan is happy and the contract is explicit).
   - Re-check `v2 = tv->version`; if `v1 != v2` or locked, abort.
   - Record `(tv, v1)` in the read set.

   **Pointee immutability contract.** The version stamp protects the
   pointer `tv->value`, not the bytes it points at. For TVars whose
   value is a boxed multi-word payload (struct, vector, HAMT node), the
   pointee must be treated as immutable after publication -- writers
   allocate a fresh payload and swap the pointer. This is already how
   the existing TVar callers behave, but the TL2 design makes it
   load-bearing, so we state it explicitly.

   **Abort channel (resolves G9).** `tur_tvar_read` currently has no
   error return. Add `bool aborted` to `STM_Transaction`; on a read-set
   conflict, set `tx->aborted = true` and return NULL. `tur_atomically`
   must then treat "aborted mid-body" as "restart the closure," not
   "propagate NULL to the caller" (today's `stm.c:355-358`). Codegen
   sites that consume `tur_tvar_read` (`emit_expr.c:4771-4831`,
   `elab_concurrent.c:702+`) keep working unchanged because they
   already run inside the `tur_atomically` retry loop -- the loop just
   restarts before the NULL escapes.
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
   5. Fire commit-defers via `tur_stm_fire_commit_defers` (resolves G7).
      Do this before unlocking so observers waking on the bucket cond
      see a fully-committed state.
   6. Signal waiters on each written TVar's bucket cond (see retry).
   7. Unlock buckets in reverse order.

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
touched, and snapshots each bucket's `commit_seq`. It then needs to
wake when *any* of those `commit_seq` values advances.

**Don't try to `pthread_cond_wait` on each bucket cond serially** --
holding bucket X's mutex and waiting on its cond does not observe a
wake on bucket Y. By the time the retrier finishes waiting on X and
moves to Y, Y's wake is already lost. A per-`retry()` helper thread
per parked bucket adds a thread-spawn per retry, which is worse than
today's stub.

For v1, use a **single global retry cond** on `STM_State`:

```c
typedef struct STM_State {
    _Atomic uint64_t version_clock;
    pthread_mutex_t  retry_lock;          /* guards retry_seq + retry_cond */
    pthread_cond_t   retry_cond;
    _Atomic uint64_t retry_seq;           /* bumped on every commit */
    STM_LockBucket   lock_buckets[STM_NUM_LOCK_BUCKETS];
} STM_State;
```

`retry(tx)`:
1. Lock `retry_lock`; snapshot `seq0 = retry_seq`; record the set of
   distinct buckets touched by the read set.
2. `pthread_cond_wait(&retry_cond, &retry_lock)` until either
   `retry_seq != seq0` AND at least one of the recorded buckets'
   `commit_seq` has advanced past its snapshot.
3. Unlock and re-run the closure.

Commit bumps the touched buckets' `commit_seq` (inside the bucket
lock), then takes `retry_lock` and `cond_broadcast(&retry_cond)`. The
per-bucket filter keeps a commit on bucket Z from waking a retrier
that only watches X/Y -- one extra check, no lost wakeups, no helper
threads. Spurious wakes are harmless: the retrier re-runs the closure,
which either commits or re-parks.

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
one made atomic. Per-TVar memory drops by one `pthread_mutex_t` plus
one `pthread_cond_t` -- on glibc that's roughly 90 bytes per TVar; on
macOS/musl somewhat less. Exact figure isn't load-bearing; the point
is we stop paying mutex+cond per TVar.

## Work items

| # | Item | File(s) | Notes |
|---|------|---------|-------|
| F0 | Decide tur+turi parity (see Open Question above). Default: bring `turi` along. | this doc | Gate before F-work; if punted, F11 must call out the semantics divergence. |
| F1 | Add `version_clock` to `STM_State`; init in `tur_stm_init`. Verify `tur_stm_init` is actually called from runtime startup; if not, wire it up. | `src/runtime/stm.{c,h}` | `_Atomic uint64_t`, start at 0. Acquire on load, acq_rel on commit-time fetch_add. |
| F2 | Add `commit_seq` to `STM_LockBucket`; add `retry_lock`/`retry_cond`/`retry_seq` to `STM_State`. | `stm.{c,h}` | Init/destroy alongside `lock`. (Replaces the per-bucket-cond sketch -- see retry section.) |
| F2b | Fix the bucket hash to shift past alignment bits, e.g. `(addr >> 4) & (N-1)`. | `stm.c:62` | Resolves G8; without this, striping is degenerate. |
| F3 | Strip `lock`, `cond`, `lock_bucket`, `next_in_bucket` from `TVar`. | `stm.{c,h}`, `tur_tvar_new`, `tur_tvar_free` | Wipes G4. Keep `tur_tvar_free` taking the bucket lock so it can't race a concurrent commit on a sibling TVar in the same bucket. |
| F4 | Delete `STM_State.global_lock` and all references. | `stm.{c,h}` | Wipes G5. |
| F5 | Delete `STM_Transaction.parent`. | `stm.h` | Wipes G6; doc already says no nesting. |
| F6 | Make `tv->version` atomic; also atomic-load `tv->value` on the read fast path. | `stm.c`, any direct `tv->version` consumer | `atomic_load_explicit(memory_order_acquire)` on read, `release` on write. Pointer is naturally word-atomic on 64-bit, but spell it out for TSan. |
| F7 | Implement TL2 read path in `tur_tvar_read`. | `stm.c` | Adds `read_stamp` + `aborted` to `STM_Transaction` (set in `tur_stm_new_transaction`). |
| F7b | Wire the abort channel through `tur_atomically`: aborted-mid-body restarts the closure instead of returning NULL to the caller. | `stm.c:326-381` (esp. 355-358) | Resolves G9. |
| F8 | Rewrite `tur_stm_commit` per the protocol above; fire commit-defers inside the locked region. | `stm.c` | Replaces lines 230-272. Resolves G7. |
| F9 | Rewrite `tur_stm_retry` to use the global retry-cond + per-bucket `commit_seq` filter. | `stm.c` | Replaces lines 288-311. |
| F10 | Bump per-touched-bucket `commit_seq` (inside bucket lock) + bump `retry_seq` and `cond_broadcast(&retry_cond)` after unlock. | `stm.c` | Pair with F9. |
| F11 | Update `stm-guide.md` Limitations section: drop the global-lock bullet, replace with the real list (no nesting, no in-transaction I/O, fixed-size read/write/defer sets, pointee-immutability contract for boxed TVar values). | `docs/guides/stm-guide.md` | Mirror in `stm-tutorial.md`. If F0 punts `turi` parity, add a "compiled vs interpreted" divergence note. |
| F12 | Delete stale "Phase 20 / Phase 21" comments in `stm.h`, replace with a one-paragraph note describing TL2. | `src/runtime/stm.h` | Mirror in `stm.c` header comment. |
| F13 | (Conditional on F0=parity.) Port TL2 read path + global version clock to `turi`'s STM. | `src/turi/eval.c` (~1888-1950, 7100-7180) | Smaller surface than `tur` -- no buckets to thread, just the version-stamp + read-stamp + abort flag. |

F1-F5 are mechanical and independent; do them first to give F6-F10 a clean
slate. F2b can ride with F3. F11-F12 land in the same PR as F6-F10 so the
docs and header don't lie about the runtime. F13 lands in the same PR if
F0 picks parity.

## Testing

Existing fixtures under `tests/fixtures/stm-*` are the baseline. Be aware
that most of those run through `turi` -- if F0 punts `turi` parity, none
of the new fixtures below will exercise the rewritten `tur` path unless
they are compiled (drop `requires.interp`, ensure they go through
`emit-c`/`build`). Either way, add:

- **TL2 read invalidation.** Two transactions T1 (reads `a`, sleeps,
  writes `b`) and T2 (writes `a` and commits during T1's sleep). T1
  must abort and retry; assert it eventually commits with the *new*
  value of `a` visible. Run under both `tur` and (if F0=parity) `turi`.
- **Retry wakes across buckets.** A blocked retrier on TVars in buckets
  X and Y must be woken by a commit that touches either -- and a
  commit that touches *only* bucket Z must NOT wake it (verifies the
  per-bucket `commit_seq` filter, not just the global broadcast).
- **Value-tearing / pointee immutability.** A TVar holding a pointer to
  a multi-word struct: writer allocates a fresh struct and swaps the
  pointer (correct); a second variant mutates the existing struct in
  place (incorrect) and must be caught by either TSan or a torn-read
  assertion. Documents the contract spelled out in the design section.
- **Commit-defer firing.** A transaction registers an on-commit defer
  via the existing API; assert it fires exactly once after commit and
  never after an abort. Guards against G7 regressing.
- **Lock ordering.** Two committers whose write sets are reverse-ordered
  pairs of TVars must not deadlock under stress. Use ~10^4 iterations
  (not 10^5) so the TSan-instrumented variant fits inside the 10s
  per-fixture default, or set `expected.timeout` explicitly.
- **`requires.tsan` variant.** Mark the multi-threaded fixtures so the
  TSan run exercises them; the read/write-set fast paths should be
  race-free under TSan once F6 is in.

The compiled fixtures should run under `bash tests/run.sh` and the TSan
suite without `requires.dedicated-runner`. Interpreter variants only
make sense if F0 picks parity.

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
