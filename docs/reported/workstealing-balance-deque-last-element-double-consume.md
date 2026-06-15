# `workstealing-balance` fixture: last-element pop/steal race double-consumes an item (`items-processed: 9`)

**One-line summary:** The inlined work-stealing deque in
`tests/fixtures/workstealing-balance/input.tur` uses a non-canonical
Chase-Lev `pop` that omits the single-element CAS arbitration, so the
owner's `bwsd-pop` and a thief's `bwsd-steal` can both take the *last*
element. That double-increments the shared counter and corrupts
`top`/`bottom` (size underflow), making the program intermittently print
`items-processed: 9` (or higher) instead of the expected `8`.

**Severity:** Medium for the suite, low for product impact. This is a
**test fixture**, not shipped `stdlib`/compiler code, so no user-facing
artifact is affected. But it is a genuine concurrency bug, not timing
noise: the fixture is non-deterministic and intermittently FAILs CI under
CPU saturation, which erodes trust in the suite and can mask a real
regression behind a "probably just flaky" dismissal. It also models an
*incorrect* work-stealing deque that a reader might copy. Per CLAUDE.md
("a test that surfaces real broken behavior is a finding, not a malformed
test"), it is filed rather than silently re-run until green.

## Repro / evidence

- A full `bash tests/run.sh` on a saturated 4-core box produced:
  ```
  FAIL workstealing-balance -- stdout mismatch
  ```
  with the captured `tests/fixtures/workstealing-balance/actual.stdout`:
  ```
  items-processed: 9
  ```
  vs `expected.stdout`:
  ```
  items-processed: 8
  ```
- Standalone the bug is rare: 200/200 single-process runs printed `8`,
  and a stress harness of 2560 runs at 64-way concurrency on 4 cores also
  stayed at `8`. The single observed failure came from the real suite
  interleaving (the `timeout(1)` wrapper + `xargs` workers under genuine
  core saturation), which matches a narrow last-element race window.
- A count **above** the 8 items ever pushed is only explainable by an
  item being consumed (and counted) more than once -- a logical proof the
  race exists independent of how often it reproduces.

## Observed vs expected

- **Observed:** intermittently `items-processed: 9` (or potentially
  higher) -- the shared `counter` is incremented more times than there
  are items, because one physical slot is processed by both `pop` and
  `steal`.
- **Expected:** exactly `items-processed: 8`, deterministically, on every
  run regardless of scheduling or load.

## Root-cause analysis

`tests/fixtures/workstealing-balance/input.tur`:

- `bwsd-pop` (owner / bottom side, lines ~67-84):
  ```c
  size_t b = load(bottom, ACQUIRE);
  size_t t = load(top,    ACQUIRE);
  if (b == t) return NULL;        /* empty */
  b--;
  void *item = dq->buffer[b & dq->mask];
  store(bottom, b, RELEASE);      /* <- no CAS on top for the last element */
  return item;
  ```
- `bwsd-steal` (thief / top side, lines ~87-106) does CAS `top` on the way
  out, which is correct -- but `pop` does **not** participate in that
  arbitration.

With a single element (`top == t`, `bottom == t+1`) the two sides can
interleave so both return `buffer[t & mask]`:

1. `pop`: reads `b=t+1`, `t_local=t`; `b != t`; `b-- => t`; reads
   `item = buffer[t & mask]`; (not yet stored `bottom`).
2. `steal`: reads `top=t`, `bottom=t+1`; not empty; reads the **same**
   `buffer[t & mask]`; `CAS(top: t -> t+1)` succeeds; returns the item.
3. `pop`: stores `bottom=t`; returns the **same** item.

Both workers then `__atomic_add_fetch(&counter, 1)` for the one item.
Worse, `top` is now `t+1` while `bottom` is `t`, so `bottom - top`
underflows (`size_t` wraps huge); the next `pop` sees `b != t`, decrements
into a stale slot, and can over-count further -- consistent with `9` and
in principle `10+`.

The canonical Chase-Lev deque avoids exactly this by having `pop`
decrement `bottom` first, fence, re-read `top`, and -- when only one
element remains (`t == b`) -- **CAS `top` itself** to race the thief,
restoring `bottom` afterward. This fixture's `pop` skips all of that, so
the last element is unprotected.

## Proposed fix directions

### Option A (recommended) -- make `bwsd-pop` a correct Chase-Lev pop

```c
size_t b = load(bottom, RELAXED) - 1;
store(bottom, b, RELAXED);
atomic_thread_fence(SEQ_CST);
size_t t = load(top, RELAXED);
void *item = NULL;
if (t <= b) {                      /* non-empty */
    item = dq->buffer[b & dq->mask];
    if (t == b) {                  /* last element: arbitrate with steal */
        if (!CAS(top, &t, t + 1, SEQ_CST, RELAXED)) item = NULL; /* lost */
        store(bottom, b + 1, RELAXED);   /* restore */
    }
} else {                           /* empty */
    store(bottom, b + 1, RELAXED); /* restore */
}
return item;
```

`bwsd-steal` already CASes `top`; align its orderings with the above. This
makes the deque correct under TSan and removes the over-count entirely.

### Option B -- if the fixture only needs to prove "work distributes"

If the intent is just "both threads make progress and all 8 items are
processed exactly once," a single shared deque guarded by one mutex (or a
correct MPMC queue) is sufficient and deterministic, at the cost of not
exercising a real lock-free deque. Prefer A if the point is to test the
work-stealing algorithm itself.

### Not acceptable

Bumping `expected.stdout` to accept `9`, adding a retry loop, or marking
the fixture `requires.tsan`/skip to dodge the failure -- that hides a real
double-consume bug behind a green check.

## Validation

- Apply Option A; rebuild and run the fixture in a loop under load
  (e.g. `for i in $(seq 1 500); do ./wsb; done` concurrently across all
  cores) -- every run must print `items-processed: 8`.
- Run under `TUR_TSAN=1 bash tests/run.sh` (ThreadSanitizer): the fixed
  deque must be race-free; today's version is expected to trip TSan on the
  shared-slot access and the `top`/`bottom` underflow.
- Full `bash tests/run.sh` stays green with no intermittent FAIL.

## Notes

Surfaced while integrating the `make-struct` phantom-typeparam codegen
fix: a full-suite run reported `FAIL workstealing-balance -- stdout
mismatch` with `actual.stdout: items-processed: 9`. The make-struct change
touches `src/compiler/types.c` only and cannot affect a hand-written
work-stealing scheduler's output; re-running `workstealing-balance` in
isolation passed 200/200, confirming the failure is this pre-existing
race, not a regression from that fix.
