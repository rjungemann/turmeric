# `httpd-async-limit` hangs on GitHub's Windows runners but passes locally

> **DIAGNOSED 2026-09-04.** Reproduced locally by pinning the process to two
> cores, exactly as the "next step" below proposed. It is **not** a timing
> flake and **not** a blocked reactor: the process is SPINNING. Two distinct
> defects, one in the fixture and one in the reactor, are described under
> "Diagnosis" at the end. The reactor one affects real servers, not just this
> fixture.

**Summary:** The fixture produces no output and is killed by the per-fixture
timeout on `windows-latest` -- at 10s and again at 30s, so it is hanging, not
merely slow. The same fixture passes on a local Windows box (4/4 in isolation,
and in full-suite runs). Cause not yet identified.

**Severity:** Low. One fixture, skipped via `requires.win-concurrent-loopback`
so it does not hold the Windows CI leg red. But it is the only known case where
Windows CI and a local Windows machine disagree in this direction, so it is worth
understanding rather than leaving indefinitely skipped.

**Platform:** Windows CI runners specifically. Not reproduced locally.

## What it does

`tests/fixtures/httpd-async-limit` drives several concurrent loopback HTTP
requests against the async server and asserts how many are rejected as `busy`
once the concurrency limit is reached:

```
ok=2
busy=2
handler-ran=2
done
```

So it needs multiple connections in flight *simultaneously*; a server that
serialises them produces `busy=1` (observed once locally, under load) and one
that never completes a handshake produces nothing at all (observed on CI).

## Evidence

| where | result |
| --- | --- |
| local Windows, isolation | 4/4 pass |
| local Windows, full suite | pass |
| local Windows, full suite under load | once `busy=1` (wrong count, ran to completion) |
| `windows-latest`, `expected.timeout` default 10s | `timed out (>10s)`, no output |
| `windows-latest`, `expected.timeout` 30s | `timed out (>30s)`, no output |

The two CI runs are the important pair: tripling the timeout changed nothing and
produced no partial output, which rules out "slow runner" and points at a hang
before the first response.

## Suspects, untested

1. **Runner network stack / firewall.** The suite exports `TUR_BIND_LOOPBACK=1`
   so listeners bind `127.0.0.1`; something about the runner's loopback or
   Defender profile could stall the connect. Cheapest probe: does any *other*
   `httpd-*` fixture make concurrent connections? They pass, which argues the
   listener works and the *concurrency* is what differs.
2. **Core count.** GitHub's Windows runners are small. If the async reactor
   needs a thread to make progress while the main fiber blocks, a 2-core box
   could deadlock where a developer machine does not. This is the suspect the
   evidence fits best -- it explains a hang rather than a slowdown, and it
   explains why the wrong-count variant (`busy=1`) shows up locally only under
   load.
3. **Timing-dependent test design.** The fixture asserts an exact `busy` count,
   which is a race outcome, not an invariant. Even fixed, it may deserve a
   tolerance rather than an exact number.

## Next step

Reproduce with a constrained core count before changing any product code --
e.g. run the fixture under an affinity mask of two cores on a local Windows box.
If that reproduces, suspect 2 is confirmed and the reactor is the thing to look
at, not the fixture.

## Related

- [windows-hardcoded-tmp-resolves-to-drive-root.md](windows-hardcoded-tmp-resolves-to-drive-root.md) -- the other finding from the same first CI run
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)


## Diagnosis (2026-09-04)

### Reproduced

Pinning the process to two cores reproduces it on a 12-core box:

| cores | result |
| --- | --- |
| 12 (unpinned) | 5 / 5 pass |
| 2 (`ProcessorAffinity = 3`) | 3 of 5 hang on the first sample; roughly 1 in 5 to 1 in 12 thereafter |

So suspect 2 from the list above -- runner core count -- is confirmed, and
suspects 1 and 3 are not needed to explain it.

### It is spinning, not blocked

The first thread dump looked like a blocked reactor: the server thread sat in
`select_poll` with `tur_reactor_poll(rp, timeout_ms=-1)`, an infinite wait, and
main sat in `chan_recv`. That reading was wrong.

Dumping the reactor's source table at the moment of the hang shows two ACTIVE
timer sources, so `cap_timeout` clamps the infinite timeout to the next
deadline and `select` returns promptly. The stack was simply caught mid-poll.

Attaching twice, five seconds apart, settles it:

```
t0:        SOURCES=3953
t1 (+5s):  SOURCES=5285
```

~1330 new sources in five seconds is ~133 park/unpark cycles per second, which
is exactly the cadence of the handler's `(httpd-await-timer c 10)`. The process
is running flat out, not waiting.

### Defect 1 -- the fixture's gate can never open if a client drops out

The handler holds its in-flight slot with

```turmeric
(while (< (read-counter busy) 2)
  (httpd-await-timer c 10))
```

`busy` is incremented by a client that receives a 503. But the client exits
silently on two paths that do NOT count: a failed `connect()` returns `NULL`,
and the 8s `SO_RCVTIMEO` backstop makes a slow exchange give up the same way.
By the time of the hang **all four client threads have exited** -- the thread
dump shows only main, the server, and Windows thread-pool workers.

So if either over-cap client drops out, `busy` never reaches 2, both handlers
spin forever on their timers, neither sends to `donech`, and main blocks in
`chan_recv` for good. The fixture's header explains that gating on observed
rejections (rather than a fixed delay) makes the counts robust against
scheduling -- which it does -- but there is no backstop for a client that never
reports at all.

### Defect 2 -- reactor source slots are never reused (this one is not test-only)

`alloc_source` is documented as "append a fresh source slot, growing the array
if needed", and that is literally what it does. `tur_reactor_remove` deactivates
a source but its slot is never reclaimed, so `r->sources_len` only ever grows:
4131 and 5285 were observed here, with `sources_cap` already doubled to 8192.

Two consequences, both real outside this fixture:

1. **Unbounded memory growth** for any fiber that parks in a loop -- which is
   the normal shape of an await inside a long-lived connection handler.
2. **Every poll costs O(sources ever created), not O(active).** `cap_timeout`
   and `tick_timers` each walk the whole array on every single
   `tur_reactor_poll`. A server that has served many awaits pays that scan
   forever, so throughput degrades over uptime rather than settling.

A two-core box does not cause this; it only makes the fixture spin long enough
to make it visible.

### Fix directions

**Defect 2 is FIXED.**  `alloc_source` now recycles slots, DEFERRED by one
poll.  Under the same spin `sources_len` went from 3953 -> 5285 in five
seconds to **9 and flat**, so a poll now costs O(active) rather than
O(sources-ever-created).  Pinned by `tests/reactor_slot_reuse_unit.c`, which
fails against immediate recycling and passes against the deferred version.

The deferral is the non-obvious half.  `tick_timers` holds `src` across the
callback it runs and writes through it afterwards, so if that callback both
REMOVES a source and REGISTERS one, immediate recycling hands the freed slot
to the new registration and the post-callback write deactivates it.  Both
halves are required: an ordinary re-arming timer callback does NOT trigger
it (measured), because tick_timers deactivates a one-shot directly and never
calls `tur_reactor_remove`.  Nothing in-tree does both today --
`local_park_wake` removes but does not register -- so the deferral is
defensive against a shape the API permits.

**Defect 1 is untouched**, so the fixture still spins and stays skipped on
Windows; the spin is simply cheap and non-growing now.

For defect 2 -- reuse an inactive slot in `alloc_source` instead of appending.
Source ids come from a separate monotonic `next_id`, so a reused slot still
gets a fresh id and a stale id cannot alias it. Contained, and testable by
watching `sources_len` stay flat across a parking loop.

For defect 1 -- give the handler's wait a deadline so a missing client cannot
wedge it, and have the client count its own failure rather than returning
`NULL` silently. Both change the fixture's synchronization, so they want more
thought than the reactor fix: the current design is deliberate and the header
explains why.

Until then the fixture stays skipped on Windows via
`requires.win-concurrent-loopback`.
