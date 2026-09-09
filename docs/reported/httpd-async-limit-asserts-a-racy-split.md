# `httpd-async-limit` asserts an exact admit/reject split that is a race, and hardcodes the count that would catch it

**Severity:** low -- test-only, no product defect implicated. Worth recording
because it fails intermittently on loaded CI runners, reads as a real
regression on whatever PR happens to be running, and the one line that looks
like it corroborates the cap is a constant.

## Summary

`tests/fixtures/httpd-async-limit` starts an async server with
`max-in-flight=2` and a handler that awaits a 100ms timer, fires **four**
concurrent clients, and asserts the split is exactly `ok=2` / `busy=2`.

Whether a given client is admitted or 503'd depends on whether it connects
before or after one of the first two handlers finishes. That is a scheduling
race, not a property of the cap, so the assertion is only stable while every
client connects inside the same ~100ms window.

## Observed failure

CI run 30738162068, job 91470625668, `Test (macos-latest)`, 2026-08-02
(on PR #759, whose change is compile-time refinement discharge and cannot
reach runtime socket scheduling):

```
FAIL httpd-async-limit -- stdout mismatch
    --- tests/fixtures/httpd-async-limit/expected.stdout
    +++ tests/fixtures/httpd-async-limit/actual.stdout
    @@ -1,4 +1,4 @@
    -ok=2
    -busy=2
    +ok=3
    +busy=1
     handler-ran=2
     done
summary: 2511 passed, 1 failed
```

Every other `httpd-*` fixture passed in the same run.

## Root cause

In `tests/fixtures/httpd-async-limit/input.tur`:

- `client-worker` sleeps 30ms before connecting
  (`struct timespec ts = { 0, 30000000L }`), so all four clients aim at the
  same instant.
- the handler awaits a 100ms timer (`(httpd-await-timer c 100)`), so the two
  admitted slots are held for ~100ms.
- the server is built with `(httpd-new-async-with-limit 0 handler 2)` and four
  clients `c1`..`c4` are spawned back to back.

A client whose `connect` lands later than roughly 130ms after start finds a
freed slot and is served, giving `ok=3` / `busy=1`. Only ~100ms of scheduling
jitter on one client is needed, which an idle laptop never produces and a
loaded runner produces routinely.

### The corroborating line is a literal

```turmeric
(print-tag "handler-ran" 2)
```

`handler-ran` is not measured -- it is the constant `2`, printed
unconditionally. So it stays `2` in the failing output while `ok=3` says a
third request was actually served, i.e. a third handler ran. The field that
looks like it confirms the cap held is exactly the field that cannot observe
it being exceeded.

This is worth fixing independently of the flake: as written, the fixture
cannot fail on the property it advertises.

## Reproduction status -- honest

**Not reproduced locally.** 25 consecutive runs on an idle machine
(`TUR_TEST_FILTER='^httpd-async-limit$' bash tests/run.sh`) passed 25/25, with
`ok=2 busy=2` every time. That is consistent with the diagnosis rather than
evidence against it: the failure needs one client's connect delayed past the
100ms handler, which an unloaded machine does not do.

A contended-runner reproduction (running the fixture against a saturated CPU)
was not carried out. Anyone confirming this should load the box first; idle
repetition will not shake it out.

## Fix directions

1. **Assert the invariants, not the split.** What the cap actually guarantees
   is `ok + busy == 4` and that no more than 2 handlers are in flight *at
   once*. Both are stable under scheduling; `ok == 2` is not.
2. **Measure `handler-ran`.** Increment a counter in the handler and print it,
   so the fixture can observe the cap being exceeded instead of asserting a
   constant. Peak concurrency is the honest quantity -- a counter incremented
   on entry and decremented on exit, tracking its maximum.
3. **Or remove the race**: hold every admitted handler until all four clients
   have connected (a barrier / a channel the test releases), so the window
   cannot close early. This preserves the exact-split assertion by making it
   deterministic, at the cost of a more intricate fixture.

(1) plus (2) is the smaller change and tests more than the current fixture
does.

## Note

Do not read an isolated `httpd-async-limit` failure as a regression in a PR
that does not touch the reactor, the httpd spice, or threading. Check whether
the same commit passes on a re-run before investigating further.
