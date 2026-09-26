# `httpd-async-limit` hangs on GitHub's Windows runners but passes locally

> **RESOLVED 2026-09-26** -- see "Resolution" at the end. "Defect 1" below
> was not a fixture defect: the SERVER reset every 503 connection, and on
> Windows a reset discards the reply the client has not read yet. The 503 path
> now closes gracefully (`tur_reactor_linger_close`), the fixture counts a
> client that drops out instead of waiting on it forever, and the
> `requires.win-concurrent-loopback` skip is gone.
>
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

- [windows-hardcoded-tmp-resolves-to-drive-root.md](../archive/windows-hardcoded-tmp-resolves-to-drive-root.md) -- the other finding from the same first CI run
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

## Resolution (2026-09-26)

### Defect 1 was the server, not the fixture

The diagnosis above established that an over-cap client drops out without
being counted. It did not establish *why* a client on loopback would drop out,
and the answer is not in the fixture. Instrumenting the client to report how
each exchange ended, on a 12-core Windows 11 box, unpinned:

```
client: recv r=-1 errno=10054 total=94     <- 503 client: WSAECONNRESET
client: recv r=-1 errno=10054 total=94     <- 503 client: WSAECONNRESET
client: recv r=0  errno=0     total=104    <- 200 client: clean EOF
client: recv r=0  errno=0     total=104    <- 200 client: clean EOF
```

**Every** 503 connection ended in a reset, even on passing runs. The async
accept callback answered an over-cap connection with `send(503)` then
`close()`, without ever reading the request. Closing a socket that still holds
unread data sends RST instead of FIN. That is true everywhere. What is
Windows-specific is the next step: when the RST lands before the client has
read the reply, Windows **discards** the buffered reply, and `recv()` returns
`WSAECONNRESET` with nothing delivered.

On an unpinned box the client is already blocked in `recv()` when the 503
arrives, so it reads the bytes first and only the *next* `recv()` sees the
reset. Pinned to two cores (`ProcessorAffinity = 3`), the client thread is often
descheduled between `send()` and `recv()`, and by the time it reads, the reset
has already landed:

```
client: recv r=-1 errno=10054 total=0      <- 503 lost to the reset
client: recv r=-1 errno=10053 total=0      <- WSAECONNABORTED, same story
client: recv r=-1 errno=10060 total=0      <- 200 clients: the 8s SO_RCVTIMEO
client: recv r=-1 errno=10060 total=0         backstop, because the handlers
                                              never stopped waiting for busy=2
```

12 of 20 runs hung that way. That is the whole mechanism: a 503 lost to the
reset, `busy` stuck below 2, both handlers spinning on their timers, main
blocked in `chan-recv`.

This was a real server bug, not a test artefact: any client of an over-cap
`httpd-new-async-with-limit` server on Windows could get a connection reset in
place of the 503 it was sent.

### The fix

- **`tur_reactor_linger_close(r, fd, timeout_ms)`** (`src/async/reactor.c`),
  a lingering close that never blocks the reactor. It shuts down the send side
  (the client gets the reply and then FIN), sets the fd non-blocking, and
  drains and discards whatever the client still sends. It closes the fd when
  the client closes its end, when an error occurs, or when the timeout expires.
  The linger is an fd source plus a one-shot timer with inline, disowned boxes,
  so no fiber is needed: spawning a 1 MB fiber stack for every *rejected*
  connection would work against a cap whose job is to bound per-connection
  cost. The reactor keeps a list of lingering sockets, so `tur_reactor_free`
  closes any that are still open. `TUR_LINGER_MAX` (1024) bounds how many
  descriptors can linger at once; past it, a close is abortive again.
- **The 503 path** in `httpd-async-accept-cb` (`stdlib/httpd.tur`) calls it
  with a 2 s deadline instead of `close()`.
- **The fixture** now counts its own failures. A client that ends with neither
  a 200 nor a 503 (connect failed, timed out, or was reset) bumps a `lost`
  counter, and the handlers' gate opens on `busy + lost >= 2`. A drop-out
  therefore shows up as a `lost=N` line in the diff instead of a timeout with
  no output. `lost` is printed only when non-zero, so `expected.stdout` is
  unchanged.
- **`tests/reactor_linger_unit.c`** (ctest `tur_reactor_linger_unit`) pins the
  primitive. The server replies to a request it has not read, lingers, and the
  client reads only after a delay. The test checks that the client gets the
  reply and then EOF rather than a reset, that the client's close ends the
  linger early, that the timeout ends it otherwise, and that teardown closes a
  socket that is still lingering.

### Verified (Windows 11, MSYS2/UCRT64, gcc 16.1, Debug)

| run | result |
| --- | --- |
| instrumented fixture, old server, 2 cores | **12 of 20 hung** |
| instrumented fixture, new server, 2 cores | 0 of 25 bad; every 503 client now ends `r=0` (EOF), not `10054` |
| the fixture itself, new server, 2 cores | 0 of 25 bad |
| the hardened fixture against the OLD server, 2 cores | 0 hung, 5 of 15 `busy=1 ... lost=1` -- a legible failure instead of a hang |
| `tur_reactor_linger_unit` | passes |
| `tur_reactor_linger_unit` with the linger mutated to a plain `close()` | **fails 3 of 3** ("the peer reads the reply and then EOF, not a reset") |

The last row is the regression check: on Windows the unit test is
deterministic against the old behaviour, because the client reads only
after the reset has had time to land.

The `requires.win-concurrent-loopback` marker and both of its guards in
`tests/run.sh` are removed. This fixture was the marker's only user.

### Not done

The **normal** response path (`httpd-async-fiber-body`, and the blocking
`httpd-handle`) still ends with a plain `close()`. That is safe for a request
the server has read in full, which is the common case. It is not safe when the
client sends more than the server read: a body larger than its
`Content-Length`, pipelined bytes after a `Connection: close` request, or an
early error response before the body arrives. Any of those resets the
connection and can cost a Windows client the response. Routing the async path
through `tur_reactor_linger_close` is a one-line change. The blocking path has
no reactor and would need a bounded blocking drain instead. Neither is
fixture-visible today, so neither was changed here.
