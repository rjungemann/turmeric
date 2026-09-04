# `httpd-async-limit` hangs on GitHub's Windows runners but passes locally

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
