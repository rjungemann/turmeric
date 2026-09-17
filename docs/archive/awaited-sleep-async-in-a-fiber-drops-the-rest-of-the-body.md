# `(await (sleep-async n))` inside an interpreter fiber silently drops the rest of the fiber body

**RESOLVED 2026-09-17** -- see Resolution at the end. The suspected root cause
below is close but not right about the mechanism; the correction is there too.

**Severity: medium.** Under `tur --interpret`, an `async` fiber that sleeps via
`(await (sleep-async n))` never executes anything after the sleep. The fiber's
future resolves anyway, so `(await t)` on the spawner side returns normally and
the program exits 0. No error, no warning -- the tail of the fiber body is just
gone.

Writing the same sleep as a bare `(sleep-async n)` works. The difference is the
`await`.

## Repro

```turmeric
(defn main [] : int
  (let [t (async (fn [] (println "before") (await (sleep-async 700)) (println "woke")))]
    (await t))
  (println "done")
  0)
```

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret t-sleep2.tur
before
done                    <-- "woke" never printed; exit 0
real 0m0.853s           <-- the 700ms sleep did happen
```

Control -- identical program with the `await` removed:

```turmeric
(let [t (async (fn [] (println "before") (sleep-async 700) (println "woke")))]
  (await t))
```

```
before
woke
done                    <-- correct
real 0m0.775s
```

Second control -- the same `(await (sleep-async 700))` in the **main** context
is fine (`before` / `woke`), so it is specific to fiber context.

Measured against `./build/tur` at v0.48.0 (2026-09-16).

## Root cause (suspected -- not yet confirmed at the C level)

`native_sleep_async` in `src/turi/fiber.c` is already context-sensitive:

```c
TuriFiber *cur = env->current_fiber;
if (!cur) {
    /* Main context: return future so (await (sleep-async ms)) works. */
    return turi_sleep_async(env, ms);
}
/* Inside a fiber: create future, add timer, suspend until it fires. */
...
swapcontext(&cur->ctx, &env->sched_ctx);
/* Resumed: timer fired. */
return turi_nil();
```

In a fiber it **already blocks** and hands back `turi_nil()`, so the surrounding
`await` is applied to nil rather than to a future. The control above shows the
blocking half is correct -- the fiber does resume and does run its tail. So the
defect is in what `await` does to a non-future value in fiber context: it appears
to unwind or complete the fiber rather than pass the nil through. The
`turi_fiber_reclaim_if_done` path and `await`'s fiber arm are where to look
first.

Worth confirming before fixing: whether the fiber is reclaimed early, or the
`await` longjmps, and whether other `await`-on-a-non-future shapes have the same
effect (a plausible generalisation -- this need not be sleep-specific).

## Why this is filed with the session work

It makes a shipped session fixture pass for the wrong reason.
`tests/fixtures/session-timeout-expired-turi/input.tur` spawns its peer as:

```turmeric
(async (fn []
  (await (sleep-async 200))
  (let [s (send s 99)] (close s))))
```

That fiber never reaches the `send`. The fixture asserts `timeout`, and it gets
`timeout` because **no value is ever deposited** -- not because the 50ms deadline
was observed. It would pass identically with `recv-timeout` stubbed out to always
return the Right branch, so it provides no coverage of the timed wait it was
written to test. (The timed wait itself is correct in main context; it is broken
in fiber context, filed separately as
[turi-fiber-recv-timeout-ignores-its-deadline](turi-fiber-recv-timeout-ignores-its-deadline.md).)

It also produced two false leads while investigating session parity: with the
awaited sleep in place, a correct program whose peer sleeps before sending
reports `eval: session recv deadlocked (no sender)` (exit 1) or livelocks at
100% CPU. Both vanish with the bare `sleep-async`. The interpreter's session
deadlock detection is **sound** -- it was reporting a peer that genuinely never
sends. Anyone re-deriving this should not go looking for a false-positive
deadlock in the channel runtime; there is none.

## Fix

Correct the `await` fiber arm, then drop the redundant `await` from
`session-timeout-expired-turi` so it tests the deadline, and add a plain
regression fixture for the repro above.

## See also

- `src/turi/fiber.c` -- `native_sleep_async`, `turi_fiber_reclaim_if_done`.
- `src/turi/eval.c` -- the `await` / `EX_ASYNC` paths.
- `tests/fixtures/session-timeout-expired-turi/` -- the vacuous fixture.

## Resolution (2026-09-17)

### Correction to the suspected root cause

The suspicion above -- "`await` ... appears to unwind or complete the fiber" --
points at the right line and the wrong mechanism. Nothing unwinds and nothing
is reclaimed early. `EX_AWAIT` (`src/turi/eval.c`) simply fails its tag check:

```c
if (fv.tag != TURI_FUTURE)
    return turi_errorf("eval: await: expected a future, got tag %d", fv.tag);
```

That `TURI_ERROR` is an ordinary value. It propagates out of the fiber body the
way any error does -- which is why the tail never runs -- and
`async_fiber_thunk` then takes its `turi_is_error(result)` arm and **rejects**
the fiber's own future. The rejection is real; it is only invisible because the
repro discards `(await t)`. Printing it shows the whole story:

```
$ tur interpret t-diag.tur      # (println (await t)) added
before
91328185161136                  <-- the rejection, printed as a raw word
done
```

So there was a diagnostic all along, one level up from where anyone looked.

### The fix

`native_sleep_async`'s fiber arm violated its own declared contract. Its
docstring is `sleep-async : (ms :int) -> Future` and its main-context arm
returns one; the fiber arm blocked on the timer and then handed back
`turi_nil()`. Blocking early is a legitimate implementation detail -- the
control in this report proves that half was always correct -- but the VALUE has
to keep the shape the caller was promised. It now returns the (by then
resolved) future:

```c
/* Resumed: timer fired, so `f` is already RESOLVED.  Hand back the FUTURE, ... */
return turi_future_val(f);
```

`await` on an already-resolved future is free, so both spellings now work and
neither pays for the other: the awaited one resolves immediately, the bare one
still blocks and discards a future nobody reads.

**The generalisation the report asked about is real, and was fixed with it.**
`native_read_async` and `native_write_async` have the identical shape -- both
declare `-> Future`, both return `f->result` from their fiber arm -- so
`(await (read-async fd n))` inside a fiber failed the same tag check. No
fixture reaches them (they appear only in the sandboxing guides, and only as
names that get blocked), which is why this surfaced through `sleep-async`
first. Both now return the future in both contexts.

### The vacuous fixture

`tests/fixtures/session-timeout-expired-turi` is no longer vacuous, and did not
need the `await` dropped to get there -- with the fix its peer fiber does reach
its `send`, at 200ms, against a receiver whose 50ms deadline has already
fired. Probed directly rather than assumed:

```
$ tur interpret <fixture with a println added to the peer>
timeout
peer-awake      <-- the send is now reached; before the fix it never was
```

Keeping the `await` also keeps a second user of the fixed shape in the suite.

### Tests

- `tests/fixtures/await-sleep-async-in-fiber-turi/` (`requires.interp-only`) --
  the repro plus its bare-`sleep-async` control in one program, so a future
  change that fixes one spelling by breaking the other fails here.
- `tests/fixtures/session-timeout-expired-turi/` -- unchanged, now meaningful.
- All seven `tests/turi/eval-async-*.sh` ctest targets pass, including
  `eval-async-timeout`, whose test 3 is this exact shape inside `with-timeout`.
- `bash tests/run-turi.sh` -- 2118 passed, 0 failed.
  `bash tests/run.sh` -- 3031 passed, 0 failed.
