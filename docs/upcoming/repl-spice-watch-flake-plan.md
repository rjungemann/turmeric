# Plan: De-flake `tur_repl_spice_watch` CI test

> **Status:** Draft Plan
> **Last Updated:** 2026-05-28
> **Type:** Test infrastructure / CI reliability

---

## Overview

`tur_repl_spice_watch` (ctest target; script
`tests/turi/repl-spice-watch.sh`) intermittently fails under CI even though the
behavior it exercises is correct.  It passes reliably when run in isolation but
flakes when run inside the full `ctest` suite (parallel jobs, ASAN-instrumented
binary, loaded runner).  The flake is timing-based, not a real regression in
`tur repl --watch`.

This plan captures the root cause and a ranked set of fixes so the test can be
made deterministic without weakening what it verifies.

## Symptom

Under `ctest --output-on-failure` the run reports:

```
The following tests FAILED:
        11 - tur_repl_spice_watch (Failed)
```

Re-running the script standalone (`bash tests/turi/repl-spice-watch.sh`) passes
5/5 every time.  The failing scenario is one of the two FIFO-driven sessions
(`rp6-watch-edit-auto-reload` or `rp6-no-watch-skips-auto-reload`).

## Root cause

Scenarios 2 and 3 drive a live REPL through a FIFO and coordinate three actors
-- the test script, the REPL's readline loop, and the `--watch` file-change
detector -- using **fixed `sleep 1` delays**:

```sh
printf '(answer)\n' >&3   # input 1
sleep 1                   # hope: REPL printed "=> 42"
write_lib "$root" 99      # mutate src/lib.tur
sleep 1                   # hope: watcher noticed the mtime change
printf '(answer)\n' >&3   # input 2 -- expect auto-rebuild -> "=> 99"
```

Three independent races sit inside those one-second windows:

1. **REPL startup + first eval.**  Under ASAN and a loaded CI runner the REPL
   may not have started, AOT-compiled the spice into `.tur-repl-cache/`, and
   printed `=> 42` within the first second.

2. **Auto-rebuild cost.**  `--watch` reload AOT-compiles the edited spice into a
   shared library via a `cc`/`clang` subprocess.  On a busy runner that compile
   can exceed the second window between the source edit and input 2, so the
   rebuild line (`(reload) rebuilt 1 export`) and `=> 99` have not appeared when
   the test scrapes the output.

3. **mtime granularity.**  The watcher detects changes by file modification
   time.  If the test's `write_lib ... 99` lands in the same coarse mtime tick
   as the cache build (filesystem mtime resolution is 1s on some platforms,
   notably older macOS filesystems), the change can be missed entirely.

Because all three depend on wall-clock timing rather than observed state, the
test is inherently racy under load.

## Remediation options

### Option A (recommended): replace fixed sleeps with output-driven waits

Add a `wait_for <file> <pattern> <timeout>` helper that polls the REPL's
`out.log` until the expected marker appears (or a generous timeout, e.g. 30s,
elapses).  Sequence becomes:

- write input 1; `wait_for out.log '=> 42'`
- mutate source; (see Option C for the mtime guard)
- write input 2; `wait_for out.log '=> 99'` (watch case) or assert the rebuild
  line is absent after the second `=> 42` (no-watch case)

This removes all wall-clock guesses: the test advances exactly when the
observable state it needs is present, and only fails if it genuinely never
arrives.  Generous timeouts keep it fast in the common case and robust under
load.

### Option B (stopgap): scale the sleeps

Bump the delays (e.g. `sleep 3`) or scale them by a `TUR_TEST_SLOWDOWN`
env var.  Cheap, but still racy -- it only widens the window, never closes it.
Use only as a temporary mitigation if Option A is deferred.

### Option C (pair with A): guarantee a strictly newer mtime

Before the mutating write, ensure the new file's mtime is strictly greater than
the cache's last-seen mtime -- e.g. `touch` with an explicit future-ish stamp,
or detect and cross the filesystem's mtime granularity.  Closes race (3)
specifically.  Best applied together with Option A.

### Option D (product-side, larger): content-hash change detection

Make `tur repl --watch` detect changes by content hash (or mtime + size +
hash) rather than mtime alone.  This hardens the feature itself against
same-tick edits, benefiting real users on coarse-mtime filesystems, not just
the test.  Larger scope; track separately from the test fix.

### Option E (last resort): ctest retry / quarantine

Mark the test with a bounded retry (`ctest --repeat until-pass:3` or
`set_tests_properties(tur_repl_spice_watch PROPERTIES ...)`).  Reduces CI
redness but masks the race and slows the suite on flake.  Prefer A/C; consider
E only as a short bridge.

## Recommendation

Implement **Option A + Option C** in `tests/turi/repl-spice-watch.sh`: an
output-polling `wait_for` helper plus a strict-mtime guard on the mutating
write.  This makes the test deterministic without weakening its assertions and
needs no product change.  Consider **Option D** as a separate, user-facing
hardening of the watcher's change detection.

## Affected files

- `tests/turi/repl-spice-watch.sh` -- the FIFO-driven scenarios (2 and 3).
- `CMakeLists.txt:93` -- `tur_repl_spice_watch` ctest registration (only if
  Option E is used).
- The `tur repl --watch` change-detector (see
  [tur-watch-spice-plan.md](tur-watch-spice-plan.md) and
  `docs/guides/repl.md`) -- only for Option D.

## Effort

Small for Options A + C (test-only, no compiler change).  Medium for Option D
(watcher change-detection redesign).
