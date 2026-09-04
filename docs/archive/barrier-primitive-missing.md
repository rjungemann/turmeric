# No barrier primitive (barrier-new/barrier-wait)

**Severity: low** -- threading-guide.md documented one (pre-fix); nothing
existed. Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `stdlib/barrier.tur`.

## Repro

`grep -rn barrier stdlib/ src/` -> nothing.

## Resolution

`stdlib/barrier.tur`: a counting barrier over mutex + condvar, loaded
explicitly. `Barrier` is a `:linear` `defopaque` over `:ptr<void>` so it
cannot be passed where a Mutex or CondVar belongs and must be torn down
exactly once.

| | |
|---|---|
| `barrier-new [n : int] : Barrier` | releases every n-th waiter |
| `barrier-wait [^borrow Barrier] : bool` | blocks until n arrive |
| `barrier-free [Barrier] : nil` | destroy |

Three decisions worth recording:

**Reusable, not one-shot.** After releasing N threads the barrier resets, so a
phased workload (every worker finishes phase 1 before any starts phase 2) works
across rounds. That is the thing a barrier is *for*; the STM sketch the guides
pointed at counts arrivals and never resets, making it a latch.

**Sense-reversing.** Each round bumps a generation counter and a waiter sleeps
until *its* generation ends -- not merely until a broadcast arrives. Without
that, a thread that loops back around and re-enters can be released by its own
next round's broadcast, which is the classic way a hand-rolled barrier lets one
thread run a phase ahead. It also makes the wait robust to spurious wakeups,
since the predicate is a real condition rather than a flag.

**Built from mutex + condvar, not `pthread_barrier_t`.** That type is an
optional POSIX feature macOS does not ship -- the same reason
`stdlib/sync.tur` hand-rolls its semaphore instead of using `sem_t`.

`barrier-wait` returns `bool`, not `:int`: `true` for exactly one thread per
round (the arrival that tripped it, mirroring `PTHREAD_BARRIER_SERIAL_THREAD`),
which lets one thread run the between-phases work without a second lock.

The `TurBarrier` layout is declared once at file scope via `__tur_include__`
rather than repeated per inline-C body -- the HttpdConn lesson.

## Tests

`tests/fixtures/barrier-rendezvous` spawns **three real OS threads** through
**two phases**. A barrier is meaningless single-threaded, so a single-threaded
smoke test would have been decoration.

It asserts three things: all three workers ran phase 1; **none entered phase 2
early** (a worker checks `phase1_done < 3` on the far side of the barrier); and
`barrier-wait` returned `true` exactly twice, once per round.

Validated with a negative control rather than assumed: with the phase-1
`barrier-wait` removed, the fixture reports `early=2, serial=1` instead of
`0, 2` -- both signals move -- deterministically across five runs. The passing
version is likewise stable across five runs.

Suites: run.sh 2675 passed / 0 failed; run-turi.sh 1843 passed / 0 failed.

## Guides updated

- docs/guides/threading-guide.md -- the line saying to "build one from a TVar
  plus `check` ... or from a mutex + condvar + counter" is replaced by a
  Barrier section: usage, the serial-thread return, why not
  `pthread_barrier_t`, the sense-reversing note, and the free-after-release
  rule.
- docs/guides/stm-tutorial.md -- the Barrier sketch keeps its place (it is the
  right answer when the rendezvous must compose with other transactional
  state) but now points at the stdlib one first and admits it is a one-shot
  latch, since it counts arrivals and never resets.

Regenerated `stdlib/docstrings.tur` and `docs/api/`.
