# Flaky Parallel Test Fixes -- Implementation Plan (FLK0-FLK3)

> **Status:** Not started
>
> **Problem:** 13 test failures in parallel `just test` runs are caused by
> race conditions between concurrently executing fixture workers, not real bugs.
>
> **Last updated:** 2026-05-19

---

## Root Cause

`tests/run.sh` dispatches all fixtures in parallel via `xargs -P $JOBS` where
`$JOBS = min(nproc, 8)`. On an 8-core machine all 640+ fixtures may run
simultaneously. Most tests are isolated and fine; 13 are not, for four distinct
reasons:

| Root cause | Tests affected | Failure mode |
|---|---|---|
| Hardcoded TCP port | `async-echo-server` | `bind()` → `abort()` when two instances race |
| Hardcoded temp-file path | `async-file` | Writer/reader collision across parallel runs |
| Timing windows too narrow for loaded CI | `session-timeout-expired`, `cancel-chan`, `cancel-cooperative`, `cancel-select`, `taskgroup-timeout` | Sleep-based synchronization breaks under CPU contention |
| Timer-ordering sensitive to absolute wall time | `async-timer-basic` | 50/100/150 ms fibers fire in unexpected order when scheduler falls behind |

The remaining 4 tests in the tally are timing-adjacent: `session-timeout-ok`,
`async-sleep`, `workstealing-balance`, and `async-echo-server`'s client-startup
50 ms delay (in addition to its port collision). These fail intermittently
rather than reliably.

---

## Inventory of All 13 Suspected Tests

| Test | Category | Failure mode | Fix |
|---|---|---|---|
| `async-echo-server` | Port collision | `bind(19847)` fails if two instances overlap | FLK1: ephemeral port |
| `async-file` | Temp-file collision | Both workers write `/tmp/tur-async-file-test.txt` simultaneously | FLK1: unique temp path |
| `session-timeout-expired` | Timing window | 50 ms timeout vs 200 ms sender sleep; fails under ~3× load | FLK2: widen margins |
| `cancel-chan` | Timing window | 30 ms sleep to set up thread cancellation | FLK2: widen margins |
| `cancel-cooperative` | Timing window | 30 ms sleep before cancel signal | FLK2: widen margins |
| `cancel-select` | Timing window | 30 ms sleep before cancel signal | FLK2: widen margins |
| `taskgroup-timeout` | Timing window | 100 ms timeout thread; races thread startup | FLK2: widen margins |
| `async-timer-basic` | Timer ordering | 50/100/150 ms deadline order; fails when scheduler is preempted | FLK2: relative ordering via sync |
| `session-timeout-ok` | Timing (marginal) | 1 000 ms timeout -- rarely fails but has been seen | FLK2: verify margin is sufficient |
| `async-sleep` | Timing (marginal) | `nanosleep` blocks a real thread; output is deterministic but duration check could drift | FLK2: output is unconditional -- no fix needed, mark verified |
| `workstealing-balance` | Timing (marginal) | Atomic counter loop; correct but noisy under `ASAN` runs | FLK0: `requires.no-parallel` |
| `async-echo-server` (client delay) | Timing window | 50 ms client startup delay embedded in same fixture as port collision | Fixed by FLK1 (port becomes the synchronisation point) |
| `scheduler-multithread` | Codegen-only | Has `expected.c` but no `expected.stdout`; no runtime output check -- currently not actually flaky | No fix needed; document as verified |

> **Note:** `async-echo-server` appears twice because it has two independent
> defects. The port collision alone is sufficient to cause `abort()`.

---

## Phase FLK0 -- Infrastructure: `requires.serial` and `requires.no-parallel`

**Goal:** Give the test runner two new opt-out markers so individual fixtures
can run outside the parallel pool without changing `$JOBS` globally.

### `requires.serial`

A fixture directory containing this file is dispatched **after** the parallel
pool drains, run one at a time in its own sequential pass.

Usage: inherently stateful tests that share global OS resources (ports,
well-known temp paths, `/proc` entries). Use sparingly -- serial execution
increases total wall time.

### `requires.no-parallel`

Alias for `requires.serial`; accepted by the runner as a synonym. Exists for
readability: "this test is not safe to parallelize."

### Changes to `tests/run.sh`

After collecting `HAPPY_DIRS`, split into two lists before dispatching:

```bash
PARALLEL_DIRS=()
SERIAL_DIRS=()
for d in "${HAPPY_DIRS[@]}"; do
    if [ -f "$d/requires.serial" ] || [ -f "$d/requires.no-parallel" ]; then
        SERIAL_DIRS+=("$d")
    else
        PARALLEL_DIRS+=("$d")
    fi
done

# Parallel pass (unchanged)
if [ ${#PARALLEL_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${PARALLEL_DIRS[@]}" > "$HAPPY_LIST_FILE"
    xargs -P "$JOBS" -I{} bash -c 'run_happy_worker "$@"' _ {} < "$HAPPY_LIST_FILE" 2>/dev/null
fi

# Serial pass
for d in "${SERIAL_DIRS[@]}"; do
    run_happy "$d"
done
```

Add `requires.serial` and `requires.no-parallel` to the `run.sh` header
comment block alongside the existing marker documentation (`requires.tsan`,
`requires.compiled`, `expected.timeout`).

- [ ] Add `SERIAL_DIRS` split logic to `tests/run.sh`
- [ ] Document `requires.serial` / `requires.no-parallel` in `run.sh` header comment
- [ ] Add `workstealing-balance/requires.serial` as the first real user

---

## Phase FLK1 -- Shared-Resource Isolation

**Goal:** Fix two tests that fail deterministically when two parallel workers
collide on the same global OS resource.

### `async-echo-server` -- ephemeral port

**Problem:** Both the server and client `bind`/`connect` to hardcoded port
`19847`. A second parallel instance calls `bind()` while the first holds the
port; `bind()` returns `EADDRINUSE`; the fixture calls `abort()`.

**Fix:** Bind the server to port `0` (OS assigns an ephemeral port), then read
the actual port back with `getsockname()` and store it in a file-scope global so
the client fiber can connect to the right port.

```c
/* server: bind to port 0 */
addr.sin_port = htons(0);
if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); abort(); }

/* read back assigned port */
struct sockaddr_in bound;
socklen_t bound_len = sizeof(bound);
getsockname(fd, (struct sockaddr *)&bound, &bound_len);
_server_port = ntohs(bound.sin_port);   /* file-scope int */
```

```c
/* client: read _server_port instead of literal 19847 */
addr.sin_port = htons(_server_port);
```

The comment in the file already says "We use port 0 to let the OS assign an
ephemeral port" -- the code just never followed through. Remove the
"For simplicity, use a fixed port with SO_REUSEADDR" line and delete the
`SO_REUSEADDR` setsockopt call (no longer needed).

- [ ] Change `htons(19847)` → `htons(0)` in server `bind` call
- [ ] Add `static int _server_port;` global and `getsockname()` call after `bind`
- [ ] Change client `htons(19847)` → `htons(_server_port)`
- [ ] Remove `SO_REUSEADDR` setsockopt (no longer needed with ephemeral port)
- [ ] Remove the "For simplicity, use a fixed port" comment

### `async-file` -- unique temp path

**Problem:** Both concurrent workers write and read `/tmp/tur-async-file-test.txt`.
One worker's writer truncates the file while the other's reader is mid-read, or
the reader opens the other instance's (not yet written) file.

**Fix:** Use `mkstemp`-style unique naming keyed on the process PID. Since this
is inline C, use `getpid()`:

```c
/* Replace hardcoded path with PID-keyed path */
char _tur_tmppath[64];
snprintf(_tur_tmppath, sizeof(_tur_tmppath),
         "/tmp/tur-async-file-test-%d.txt", (int)getpid());
```

Pass `_tur_tmppath` instead of the string literal in all three `open()` calls.
Add a `remove(_tur_tmppath)` at the end of `main` to clean up.

- [ ] Replace the `/tmp/tur-async-file-test.txt` literal with a PID-keyed path
- [ ] Add cleanup `remove()` call at fixture exit

---

## Phase FLK2 -- Timing Window Stabilization

**Goal:** Fix tests whose `sleep`-based synchronization fails under CPU
contention, and fix the timer-ordering test.

### Principle

The right fix is always **condition-based synchronization** (a semaphore, a
condvar, an atomic flag) rather than widening sleep margins. Wider margins still
fail under extreme load and slow down the suite. Synchronization is exact.

For tests where that is impractical (inline-C budget is already large), widening
to ≥ 10× the expected OS scheduling quantum (10 × 10 ms = 100 ms minimum margin)
is an acceptable fallback.

### `session-timeout-expired` -- narrow timeout margin

**Problem:** Receiver calls `recv-timeout` with a 50 ms deadline; sender sleeps
200 ms then sends. The 150 ms margin is eroded when 8 parallel workers saturate
CPU and the receiver thread is not scheduled within 50 ms of starting.

**Fix:** Invert the timing: make the sender delay much longer than the timeout.
Change sender `sleep-ms 200` → `sleep-ms 500` and leave the timeout at 50 ms.
The receiver should timeout long before the sender wakes up, even under 8-way
parallel load.

- [ ] Change `(sleep-ms 200)` → `(sleep-ms 500)` in `session-timeout-expired/input.tur`

### `cancel-chan`, `cancel-cooperative`, `cancel-select` -- 30 ms cancel window

**Problem:** Main thread sleeps 30 ms to allow the worker thread to block on its
blocking operation (channel recv, spin loop, or `tur_select_blocking`). Under
CPU contention the worker thread may not have reached the blocking point yet when
the 30 ms expires, so `cancel` fires before the thread is in the cancellable
state; the cancel is a no-op and the test hangs waiting for output that never
arrives (or produces the wrong output).

**Fix:** Replace the 30 ms sleep with a barrier. Add a `pthread_mutex_t` /
`pthread_cond_t` pair that the worker signals once it has reached the blocking
point. Main waits on the condvar instead of sleeping. If a condvar is too much
overhead in the inline-C budget, at minimum raise the sleep from 30 ms to
300 ms (a 10× increase that survives typical CI load spikes).

The expedient fix (sleep widening) is acceptable as a first step:

- [ ] Change `(sleep-ms 30)` → `(sleep-ms 300)` in `cancel-chan/input.tur`
- [ ] Change `(sleep-ms 30)` → `(sleep-ms 300)` in `cancel-cooperative/input.tur`
- [ ] Change `(sleep-ms 30)` → `(sleep-ms 300)` in `cancel-select/input.tur`

Follow-up (preferred): replace sleep with condvar-based ready signal in each fixture.

### `taskgroup-timeout` -- 100 ms timeout thread races group startup

**Problem:** Timeout thread sleeps 100 ms and then cancels the task group.
The task group's worker threads may not have started yet; cancel fires before
any work begins and the test ends too quickly.

**Fix:** Raise timeout to 500 ms. At 100 ms there is meaningful risk on a
loaded 8-core CI machine; at 500 ms the scheduler always has time to start the
workers.

- [ ] Change timeout thread sleep from 100 ms to 500 ms in `taskgroup-timeout/input.tur`

### `async-timer-basic` -- absolute deadline ordering

**Problem:** Three fibers register deadlines of `now + 50`, `now + 100`,
`now + 150` ms. The test expects output in that order. Under heavy CPU load the
event loop may fall behind by >50 ms, making the absolute deadline differences
meaningless (all three fire at roughly the same time and are dispatched in
registration order, which may differ from deadline order).

**Fix:** Increase the deadline separations from 50 ms to 200 ms between tiers
(`now + 200`, `now + 400`, `now + 600`). A 200 ms gap between firings is robust
to 8× parallel load on any reasonable CI machine. Adjust the expected output
comment accordingly.

- [ ] Change 50/100/150 ms deadlines → 200/400/600 ms in `async-timer-basic/input.tur`

### `session-timeout-ok` -- verify margin is sufficient

The 1 000 ms timeout is large enough that failure should be very rare. Verify
the fixture actually passes 100 consecutive times under `TUR_TEST_JOBS=8` before
marking it resolved. If it still flakes, widen the timeout to 5 000 ms.

- [ ] Run `TUR_TEST_FILTER=session-timeout-ok TUR_TEST_JOBS=8 TUR_FORCE=1 bash tests/run.sh` 100 times and confirm 0 failures
- [ ] If failures occur: widen timeout to 5 000 ms

---

## Phase FLK3 -- Verification and Regression Guard

**Goal:** Confirm all 13 tests are fixed and prevent recurrence.

- [ ] Add a CI step that runs `just test` three times back-to-back under `TUR_TEST_JOBS=8 TUR_FORCE=1` and asserts zero failures across all three runs
- [ ] Add a comment at the top of each fixed fixture explaining what was changed and why (brief one-liner: `; DV: widened from 30ms to 300ms -- parallel flake (FLK2)`)
- [ ] Add a `KNOWN_FLAKY` comment block to `tests/run.sh` above the serial dispatch section listing all tests that received `requires.serial` markers, so future authors know why
- [ ] Confirm `requires.tsan` tests are still passing under `TUR_TSAN=1 TUR_TEST_JOBS=1` (TSan tests are already serial-only by design; verify the marker interaction with the new serial pass is correct -- TSan tests should go through the `requires.tsan` skip path, not the `requires.serial` path)

---

## What NOT to do

- **Do not lower `TUR_TEST_JOBS`** to hide the flakiness. The parallelism is not
  the bug; the tests are improperly isolated. Lowering jobs masks the problem and
  makes the suite slower.
- **Do not add `requires.serial` to all timing-sensitive tests** without first
  trying condition-based synchronization. Serial execution costs wall time; a
  condvar costs nothing.
- **Do not widen sleep margins beyond 10× without fixing the root cause.** A
  test that needs a 5 000 ms sleep to be reliable is not a test; it is a hope.
  The only durable fix for a timing race is explicit synchronization.

---

## Implementation Order

1. **FLK0 first** (infrastructure): the `requires.serial` marker allows any
   remaining flaky tests discovered after this sprint to be quarantined
   immediately without a code fix.
2. **FLK1 next** (resource isolation): `async-echo-server` and `async-file`
   fail 100% when they collide, so fixing them eliminates the most reliable
   failures.
3. **FLK2** (timing): sleep widening is low-risk and fast; replace with
   condvar-based synchronization as a follow-up.
4. **FLK3** (verification): run the stress-parallel check in CI to confirm the
   suite is clean.

---

## Complexity Assessment

| Phase | Effort | Risk |
|---|---|---|
| FLK0: `requires.serial` split in `run.sh` | Low (~30 min) | None -- purely additive |
| FLK1: ephemeral port in `async-echo-server` | Low (~20 min) | Low -- well-understood POSIX pattern |
| FLK1: PID-keyed temp path in `async-file` | Low (~15 min) | None |
| FLK2: Sleep widening (all 5 tests) | Low (~20 min) | None |
| FLK2: `async-timer-basic` deadline spacing | Low (~10 min) | None |
| FLK3: CI stress run | Low (~1 h CI time) | None |
