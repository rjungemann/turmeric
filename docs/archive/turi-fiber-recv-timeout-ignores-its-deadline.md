# `recv-timeout` inside an interpreter fiber ignores its deadline

**RESOLVED 2026-09-17** -- see Resolution at the end. The fix is the one this
report proposed, plus one hazard it did not name.

**Severity: medium.** Silent wrong answer, no diagnostic. A `recv-timeout`
evaluated inside an `async` fiber under `tur --interpret` waits indefinitely
for the peer's value and takes the **Left (success)** branch, no matter how far
past the deadline that value arrives. The compiled path (`pthread_cond_timedwait`)
takes the **Right (timeout)** branch. Same program, same protocol type, two
different branches.

The same op in the **main** context is correct, which is why no fixture catches
it: every shipped `recv-timeout` fixture puts the timed receive on the main
side.

## Repro

```turmeric
;; recv-timeout(50ms) INSIDE a fiber; the peer sends at 800ms.
(defn main [] : int
  (let [[r s] (make-session (Recv int (Timeout Close Close)))]
    (let [t (async (fn []
                     (match (recv-timeout r 50)
                       (Left pair) (let [[v r] pair] (println v) (close r))
                       (Right r)   (do (println "timeout") (close r)))))]
      (await (sleep-async 800))
      (let [s (send s 99)] (close s) (await t))))
  0)
```

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret t-to-fiber.tur
99                      <-- WRONG: Left branch, after waiting 800ms
real 0m0.861s           <-- it really did wait out the full 800ms
```

The control -- identical protocol, timed receive moved to the main context and
the sleeping peer moved into the fiber -- is correct:

```turmeric
(defn main [] : int
  (let [[r s] (make-session (Recv int (Timeout Close Close)))]
    (let [t (async (fn [] (sleep-async 800) (let [s (send s 99)] (close s))))]
      (match (recv-timeout r 50)
        (Left pair) (let [[v r] pair] (println v) (close r) (await t))
        (Right r)   (do (println "timeout") (close r) (await t)))))
  0)
```

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret t-to-main.tur
timeout                 <-- correct
real 0m0.844s
```

Measured against `./build/tur` at v0.48.0 (2026-09-16).

## Root cause

`session_recv_timeout` in `src/turi/eval.c` has two arms and only one of them
observes the clock:

```c
static TuriValue session_recv_timeout(TuriEnv *env, TuriChan *ch, int64_t dur_ms) {
    uint64_t deadline = turi_now_ms() + (dur_ms > 0 ? (uint64_t)dur_ms : 0);
    while (ch->data_state != 1) {
        if (ch->abandoned)              return turi_int(1);
        if (turi_now_ms() >= deadline)  return turi_int(1);
        if (env->current_fiber) {
            /* ... park cooperatively and let a woken deposit / the deadline
             * break the loop. */
            if (session_park_or_spin(env, ch, TURI_SESS_RECEIVER) != 0)
                return turi_int(1);
        } else {
            turi_sched_step(env);   /* bounded: poll_io caps each step at 50ms */
        }
    }
    ...
}
```

The main-context arm pumps a **bounded** scheduler step and returns to the top
of the loop, so `turi_now_ms() >= deadline` is re-evaluated every <=50ms. The
fiber arm calls `session_park_or_spin`, which for a fiber does a
`swapcontext` into the scheduler and **only resumes when something enqueues the
fiber again** -- and the only thing that does is `session_wake` from the peer's
deposit. Nothing arms a timer for the deadline, so the re-check at the top of
the loop is unreachable until the value the deadline was supposed to preempt
actually arrives.

The in-tree comment states the limitation ("Fiber-context timed recv would need
a scheduler timer to bound the park; the shipped variants call recv-timeout from
the main context"), so this is a known-shaped hole rather than a surprise -- but
it is a silent wrong branch for user code, not a clean error, and it is not
mentioned in [session-types-guide.md](../guides/session-types-guide.md) or
[turi-parity-guide.md](../guides/turi-parity-guide.md).

## Fix direction

The interpreter already has the timer wheel this needs: `turi_timer_add(env, ms, future)`
plus `fire_timers` in `src/turi/fiber.c`, which is what `sleep-async` uses to
bound a fiber's park. Give the fiber arm the same treatment:

1. Allocate a `TuriFuture` and `turi_timer_add(env, remaining_ms, f)`.
2. `turi_future_add_waker(f, cur)` alongside the channel's `recv_waiter`, so
   **either** the peer's deposit or the timer resumes the fiber.
3. On resume, fall back to the existing top-of-loop re-check -- it already
   returns tag 1 once `turi_now_ms() >= deadline`.
4. Cancel/detach the timer on the deposit path so a fired timer does not
   re-enqueue an already-running fiber.

Step 2 is the only genuinely new wiring; a channel currently has exactly one
`recv_waiter` slot and no notion of a second wake source.

## Test to add

`tests/fixtures/session-timeout-fiber-turi/` (`requires.interp-only`), the
repro above, `expected.stdout` = `timeout`.

**Also worth fixing while here:** `tests/fixtures/session-timeout-expired-turi`
does not actually verify the timed wait. Its peer is
`(async (fn [] (await (sleep-async 200)) (let [s (send s 99)] (close s))))`, and
per [awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body](awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body.md)
that fiber never reaches the `send` at all. The fixture prints `timeout` because
**no value is ever deposited**, which is indistinguishable from a working
deadline. Dropping the `await` around `sleep-async` makes it a real test.

## See also

- `src/turi/eval.c` -- `session_recv_timeout`, `session_park_or_spin`.
- `src/turi/fiber.c` -- `turi_timer_add` / `fire_timers` / `native_sleep_async`,
  the fiber-park-with-timer pattern to copy.
- [turi-session-expansion-plan.md](../upcoming/turi-session-expansion-plan.md) -- phase S1.

## Resolution (2026-09-17)

Implemented as proposed: the fiber arm arms one scheduler timer for the whole
remaining wait and registers the fiber as its waker alongside `ch->recv_waiter`,
so either the peer's deposit or the alarm resumes it and the top of the loop
re-checks the clock. Steps 1-3 were mechanical. Step 4 -- "cancel/detach the
timer on the deposit path" -- turned out to be two separate hazards, and the
second one is not in this report.

### Hazard A: the alarm outliving the wait (the one this report named)

Handled on every exit path by resolving the alarm future before returning.
`turi_future_resolve` runs `flush_wakers` immediately, and that function only
enqueues a fiber whose state is `TURI_FIBER_SUSPENDED`; the fiber doing the
resolving is `TURI_FIBER_RUNNING`, so its own waker is dropped rather than
re-queued. `fire_timers`' eventual visit is then a no-op on a settled future,
and frees the timer as usual. The symmetric case -- the ALARM woke us and
`ch->recv_waiter` still points at this fiber -- is cleared right after the park,
so a later deposit cannot enqueue a fiber that is already running.

### Hazard B: `session_wake` left the fiber SUSPENDED (not in this report)

`session_wake` enqueued its waiter without changing the fiber's state, which
was harmless while a channel had exactly one wake source. With a second source
it is a live double-enqueue: the peer deposits and enqueues the fiber, the
fiber is still marked `SUSPENDED` while it sits on the ready queue, and if the
alarm fires in that window `flush_wakers` passes its `SUSPENDED` guard and
enqueues the same fiber a second time. `turi_sched_enqueue` clears
`sched_next` as it appends, so the second enqueue **truncates the ready queue**
-- everything queued behind that fiber is silently lost.

Fixed by marking the fiber `TURI_FIBER_READY` before enqueueing, which is
exactly what `flush_wakers` already does; that guard is then load-bearing for
both wake paths instead of one. `flush_wakers` was the only reader of the state
in the tree, so this is a two-line change with no other consumer.

### Measurement

The repro's control -- the timed receive moved to the main context -- was
already correct and stays correct. The fiber-side repro now prints `timeout`.
Neither of those alone proves the deadline was *observed* rather than the value
merely never arriving, so the fixture asserts ORDER instead:

```turmeric
(let [t (async (fn [] (match (recv-timeout r 50) ... (Right r) (println "fiber-timeout"))))]
  (await (sleep-async 800))
  (println "main-about-to-send")
  (let [s (send s 99)] (close s) (await t)))
```

```
fiber-timeout           <-- at ~50ms
main-about-to-send      <-- at ~800ms
```

The 50ms deadline preempts a value that is still 750ms away, which is the
property this report is about and which a `timeout`-only assertion cannot
distinguish from a peer that never sends.

### Tests

- `tests/fixtures/session-timeout-fiber-turi/` (`requires.interp-only`) -- the
  ordering program above.
- `tests/fixtures/session-timeout-expired-turi/` -- the vacuous fixture this
  report flagged. It is no longer vacuous, and did not need the `await` dropped:
  see the Resolution in
  [awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body](awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body.md),
  fixed in the same change.
- `bash tests/run-turi.sh` -- 2118 passed, 0 failed.
  `bash tests/run.sh` -- 3031 passed, 0 failed.

### What this does NOT unblock

[multi-party-sessions-have-no-timed-receive](../reported/multi-party-sessions-have-no-timed-receive.md)
names this report as its step-4 prerequisite ("fix that one first"). That
prerequisite is now met -- the interpreter's binary-session timed receive is
sound in both contexts, and `router_recv` can copy the two-wake-source pattern
verbatim. Its steps 1 and 2 (global-type syntax and the projection rule) are
untouched, and step 2 is where its design risk was.
