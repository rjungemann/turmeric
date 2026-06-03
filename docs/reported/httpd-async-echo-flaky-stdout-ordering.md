---
title: httpd-async-echo intermittently fails on stdout line ordering under parallel suite load
category: Reported
severity: low (flaky test / nondeterministic ordering -- not a miscompile)
status: resolved
---

# httpd-async-echo -- flaky stdout ordering under parallel suite load

## One-line summary

`tests/fixtures/httpd-async-echo` intermittently fails with a **stdout
mismatch** in which the first two output lines are *swapped* -- the values are
correct, only their order differs. It reproduces only under the full parallel
`tests/run.sh`, never in isolation. Severity: **low** -- this is a test
determinism/ordering issue, not a codegen defect.

## Observed vs. expected

Expected (`expected.stdout`):

```
handler-ran=1
body=echo:/hi
done
```

Observed on a flaky run (`actual.stdout`):

```
body=echo:/hi
handler-ran=1
done
```

`done` is always last; `handler-ran=1` and `body=echo:/hi` swap. No value is
wrong -- the handler ran, the echo body is correct -- so this is a line-ordering
race, not a logic error.

## Reproduction

- Full suite, high parallelism: `bash tests/run.sh` -- fails intermittently
  (observed ~1 run in several; order-sensitive to scheduler contention).
- Isolated: `TUR_TEST_FILTER='^httpd-async-echo$' bash tests/run.sh` -- passes
  **3/3** (and more) deterministically.

The contrast (flaky only under load, green in isolation) is the signature of a
scheduling/ordering race rather than a compilation bug.

## Root-cause analysis (preliminary)

The fixture is an **async** server test: a single async handler responds with
`echo:<path>` but *yields* via `await-timer`, exercising the fiber/reactor
scheduler (see the fixture header comment and `httpd_run_async`). The two
swappable lines are produced on different sides of an async yield:

- `handler-ran=1` is printed from the handler-side result (`print-tag
  "handler-ran" r`), which completes only after the handler fiber is resumed
  past its `await-timer`.
- `body=echo:/hi` is printed from the response-body check.

Under parallel suite load the host is heavily contended, so the wall-clock
interleaving of the timer-driven resume vs. the body read can flip. The test
asserts a *total* stdout order over outputs that are only *partially* ordered by
the async schedule -- so a legal schedule can produce the swapped order.

This was **not** introduced by the closure-representation-unification work: the
fixture passed in the Phase B-1 final full run and passes 3/3 in isolation
after B-2; the failure is an intermittent load-dependent flake that predates and
is independent of these changes. (It was briefly suspected during B-2 because it
surfaced in a full-suite run, but isolation runs cleared the closure changes.)

## Proposed fix directions

1. **Make the assertion order-insensitive for the two racy lines.** Sort the
   handler/body lines before comparison, or split the fixture's expected output
   so the timer-gated line is checked independently of the body line.
2. **Sequence the prints deterministically in the fixture.** Have the program
   emit `handler-ran=1` and `body=...` from the same side of the `await-timer`
   yield (e.g. gather both results, then print in a fixed order), so the stdout
   order no longer depends on the scheduler.
3. **Quarantine/serialize.** If neither is desirable, tag the fixture so it runs
   without competing for the scheduler (e.g. a dedicated-runner marker), trading
   a little suite time for determinism.

Option 2 is preferred: it removes the partial-order/total-order mismatch at the
source and keeps the test exercising the async path.

## How to validate a fix

- Run the full suite repeatedly under load (e.g. `for i in $(seq 1 20); do bash
  tests/run.sh 2>&1 | grep -E '^(FAIL httpd-async-echo|summary:)'; done`) and
  confirm zero `FAIL httpd-async-echo` lines.
- Confirm the fixture still exercises the yield (the `await-timer` path is still
  taken and `handler-ran=1` still reflects a post-resume result).

## Resolution (2026-06-03)

Applied the preferred Option 2: the two racy lines are now emitted from the
**same thread in a fixed order**, removing the partial-order/total-order
mismatch at the source. The client worker no longer `printf`s `body=...`
directly from its own thread; instead it stashes the response body into a
shared static buffer (owned by a single `async-echo-body` function, written
with `do-write=1`, read with `do-write=0`). `main` prints `handler-ran=1`,
joins the client thread (whose `pthread_join` gives the happens-before for the
buffer), then prints the body and `done` -- all on the main thread, in order.

The `await-timer` yield is untouched: the handler still parks on the reactor
for ~10ms and `handler-ran=1` still reflects a post-resume result. Verified
with 8 isolated runs and a full `bash tests/run.sh` (1312 passed, 0 failed).
