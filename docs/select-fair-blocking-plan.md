# Fair Multi-Channel Blocking for `select`

**Status:** Not started. SEL0--SEL2 planned.

**Prerequisites:** Phase threading (mutex, condvar, channel primitives).

**Last updated:** 2026-05-15

---

## Summary

This document plans the upgrade of Turmeric's `select` form from v1 single-channel
blocking to true fair multi-channel blocking. In v1, when no clause is immediately
ready and no `:default` arm is present, `select` falls back to blocking on the
first channel only. This plan describes the data structures, runtime changes, and
phases required to block on all listed channels simultaneously and wake on
whichever becomes ready first.

---

## Motivation

### The v1 Limitation

The current `select` implementation tries each clause in order using the
non-blocking `try-recv` / `try-send` variants. If all clauses miss and no
`:default` is present, the runtime falls back to a plain blocking call on the
**first** clause only:

```turmeric
;; v1 behavior: only ch-a is truly awaited when nothing is ready
(let [[idx val] (select
                  (ch-a :recv)   ; <-- blocking waits here only
                  (ch-b :recv))]
  ...)
```

This has two observable problems:

1. **Starvation** -- `ch-b` can never unblock the `select` on its own, even if
   it carries higher-priority data.
2. **Incorrect semantics** -- Go, Kotlin, and most channel-based languages
   guarantee that any ready channel can unblock a `select`.

### Goals

- Any clause whose channel becomes ready unblocks the `select`.
- When multiple clauses become ready simultaneously, one is chosen uniformly at
  random (fair selection, matching Go semantics).
- No spurious wakeups: the selected clause is guaranteed to succeed.
- Backward-compatible: existing code with `:default` arms is unaffected.

---

## Design

### Core Idea: Waiter Registration

Each channel maintains a linked list of **waiters** -- threads blocked inside a
`select` waiting for that channel. When a producer sends to (or a consumer
receives from) a channel, it walks the waiter list and signals one.

```c
typedef struct TurSelectWaiter {
    pthread_mutex_t*      wakeup_mutex;
    pthread_cond_t*       wakeup_cond;
    volatile int*         selected_idx; /* shared across all clauses */
    int                   clause_idx;
    struct TurSelectWaiter* next;
} TurSelectWaiter;
```

A `select` with N clauses allocates one `TurSelectWaiter` per clause on the
stack, then:

1. Locks all N channels in a deterministic order (sorted by address, to avoid
   deadlock).
2. Does a final non-blocking scan of all clauses under those locks.
3. If still nothing is ready, registers the waiter in each channel's waiter list
   and sleeps on `wakeup_cond`.
4. The first channel that becomes ready atomically sets `selected_idx` and
   signals `wakeup_cond`.
5. On wakeup, the `select` removes its waiter from all channels, reads the
   winning clause index, and executes the clause body.

### Fair Clause Selection

When multiple channels are ready simultaneously (caught in step 2 or step 4),
the implementation picks one uniformly at random using a lightweight xorshift
PRNG seeded per-thread. This avoids systematic starvation of later clauses.

### Locking Protocol

To prevent deadlock when two threads `select` over overlapping channel sets,
channels are locked in ascending pointer order:

```c
/* sort channel pointers before acquiring */
qsort(channels, n, sizeof(void*), cmp_ptr);
for (int i = 0; i < n; i++) pthread_mutex_lock(&channels[i]->lock);
```

All locks are released before returning from `select`.

---

## Approach Evaluation

### Approach A: Waiter-List Registration (Recommended)

**Design:** As described above -- register stack-allocated waiters in each
channel's linked list, sleep on a shared condvar, wake on first signal.

**Pros:**
- O(1) wakeup path: signaling thread walks at most N waiters
- No extra threads or event loops
- Stack allocation for waiters: zero heap cost per `select`
- Deterministic deadlock avoidance via sorted lock acquisition

**Cons:**
- Channels must carry a waiter list (one pointer field added to `TurChan`)
- Locking N channels simultaneously raises lock-order complexity
- Edge case: waiter must be removed from all lists even when one wins

**Complexity:** Medium -- ~300 lines in `src/runtime/chan.c`


### Approach B: Select Thread per `select` Site

**Design:** Spawn a helper thread that polls all channels in a round-robin loop,
posts to a result queue, then exits.

**Pros:** Simpler channel struct (no waiter list)

**Cons:**
- Thread spawn overhead dwarfs any channel operation
- Helper thread holds no channel locks -- races between poll and use
- Not truly fair without additional coordination

**Verdict:** Rejected. Thread cost and correctness issues are prohibitive.


### Approach C: Epoll/kqueue Event Loop

**Design:** Model channels as file descriptors; use OS event multiplexing.

**Pros:** Very efficient for large N; used by Go's netpoller

**Cons:**
- Requires `socketpair` or `eventfd` per channel -- heavy resource cost
- Platform-specific (`kqueue` on macOS, `epoll` on Linux)
- Far exceeds the complexity budget for this feature

**Verdict:** Rejected for v1. Worth revisiting if channel count regularly
exceeds ~64 in production workloads.


### Decision: Approach A

| Criteria | A | B | C |
|---|---|---|---|
| Correctness | ✅ | ❌ | ✅ |
| Zero heap per select | ✅ | ❌ | ❌ |
| Portable (C99 + pthreads) | ✅ | ✅ | ❌ |
| Deadlock-free by design | ✅ | ✅ | ✅ |
| Implementation size | ~300 loc | ~150 loc | ~600 loc |

---

## Architecture

### Runtime Changes

**`src/runtime/chan.h`**

```c
typedef struct TurSelectWaiter TurSelectWaiter;

struct TurSelectWaiter {
    pthread_mutex_t*      wakeup_mutex;
    pthread_cond_t*       wakeup_cond;
    volatile int*         selected_idx;
    int                   clause_idx;
    TurSelectWaiter*      next;
};

typedef struct {
    pthread_mutex_t lock;
    /* ... existing fields ... */
    TurSelectWaiter* recv_waiters;  /* NEW */
    TurSelectWaiter* send_waiters;  /* NEW */
} TurChan;
```

**`src/runtime/chan.c`**

- `tur_chan_send` / `tur_chan_recv`: after making a slot available, walk the
  opposite waiter list and signal the first unselected waiter.
- `tur_select_blocking(clauses[], n)`: implements the registration / sleep /
  deregister protocol described above.

**`src/emit.c`**

- `emit_select`: when no `:default` arm is present, emit a call to
  `tur_select_blocking` instead of the current fallback to `tur_chan_recv` on
  clause 0.

### Stdlib Changes

None. The `select` surface syntax and return convention `(index value)` are
unchanged.

### File Touchpoints

```
src/runtime/chan.h   -- TurSelectWaiter struct; waiter fields on TurChan
src/runtime/chan.c   -- waiter registration, signal on send/recv, select loop
src/emit.c           -- emit_select: call tur_select_blocking when no :default
tests/fixtures/      -- new fixtures (see Phase SEL1)
docs/guides/threading-guide.md -- remove v1 limitation note once done
```

---

## Phases

### Phase SEL0 -- Runtime Data Structures

**Goal:** Add waiter infrastructure to channels without changing observable
`select` behavior.

**Tasks:**
- [ ] Add `TurSelectWaiter` struct to `chan.h`
- [ ] Add `recv_waiters` and `send_waiters` linked-list heads to `TurChan`
- [ ] Implement `tur_waiter_register(chan, waiter)` and
      `tur_waiter_remove(chan, waiter)` under the channel lock
- [ ] Implement `tur_waiter_signal_one(list)`: find first waiter where
      `*selected_idx == -1`, CAS to `clause_idx`, signal `wakeup_cond`
- [ ] Call `tur_waiter_signal_one` at the end of `tur_chan_send` and
      `tur_chan_recv`
- [ ] Verify all existing channel fixture tests still pass (`just test`)

**Exit Criterion:** No regressions; waiter lists are wired but never populated.


### Phase SEL1 -- Blocking Select Implementation

**Goal:** Replace the first-channel fallback with true multi-channel blocking.

**Tasks:**
- [ ] Implement `tur_select_blocking(TurSelectClause* clauses, int n)` in
      `chan.c`:
  - Sort clause channel pointers for lock ordering
  - Acquire all locks
  - Final non-blocking scan; if a clause wins, release locks and return index
  - If multiple clauses ready, pick one uniformly at random (xorshift32)
  - Register one `TurSelectWaiter` per clause; sleep on shared condvar
  - On wakeup, deregister all waiters; return winning index
- [ ] Update `emit_select` in `emit.c` to emit `tur_select_blocking` call
      when no `:default` arm is present
- [ ] Add fixture `tests/fixtures/select-fair-block/`:
  - Two goroutine-style threads each sending to separate channels
  - `select` with no `:default` must receive from whichever thread sends first
  - Assert both channels are eventually selected across many iterations

**Exit Criterion:** `select-fair-block` fixture passes; no regressions.


### Phase SEL2 -- Fairness Verification and Cleanup

**Goal:** Confirm statistical fairness and remove the v1 limitation note.

**Tasks:**
- [ ] Add fixture `tests/fixtures/select-fairness/`:
  - 1000 iterations of a `select` over two always-ready channels
  - Assert each channel selected between 30% and 70% of the time
- [ ] Add fixture `tests/fixtures/select-n-way/`:
  - `select` over 4 channels; senders stagger send timing
  - All 4 channels must be selected at least once across 1000 iterations
- [ ] Stress-test under thread sanitizer (`clang -fsanitize=thread`)
- [ ] Remove the v1 limitation note from `docs/guides/threading-guide.md`
- [ ] Update `CHANGELOG` / release notes

**Exit Criterion:** All fairness fixtures pass; no data races under TSan.

---

## Open Questions

1. **Waiter cap:** Should `select` cap the number of clauses (e.g., 64) to
   bound lock-acquisition cost? Current plan: no cap, document O(N log N)
   locking behavior.

2. **Send-side fairness:** The plan covers `:recv` clauses. `:send` clauses
   need symmetric `send_waiters` lists. Confirm this is included in SEL1 scope.

3. **Cancellation:** If the thread is cancelled (future feature) while sleeping
   in `tur_select_blocking`, waiters must be deregistered. Pin this to the
   cancellation design when that feature is planned.

4. **WASM:** `pthread_cond_timedwait` is available under Emscripten with
   `-pthread`. Verify `tur_select_blocking` compiles cleanly for the WASM
   target before closing SEL2.

---

## Related Work

| System | Select semantics |
|---|---|
| Go | Uniform random among ready cases; true multi-channel block |
| Rust `tokio::select!` | Biased toward first arm by default; `select_biased!` exists |
| Erlang `receive` | Pattern-based; fair across all matching messages in mailbox |
| POSIX `pselect` | File-descriptor multiplexing; model for Approach C above |

---

## Summary

**Recommendation:** Implement Approach A (waiter-list registration) across
phases SEL0--SEL2.

**Next step:** Begin Phase SEL0 by adding `TurSelectWaiter` to `chan.h` and
wiring waiter signals into `tur_chan_send` / `tur_chan_recv`.

**Effort estimate:** SEL0 ~1 day, SEL1 ~2--3 days, SEL2 ~1 day.
