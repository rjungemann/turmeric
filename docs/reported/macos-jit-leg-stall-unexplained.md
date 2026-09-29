# `JIT engine (macos-latest)` stalls for the whole job budget, cause unknown

**Severity: medium.** It gates: `JIT engine (macos-latest)` is the one JIT leg
that is not `continue-on-error`, so a stall fails the run. Four occurrences to
date, the most recent
[run 36385448273](https://github.com/rjungemann/turmeric/actions/runs/36385448273)
on 2026-09-28 (rjungemann/turmeric#953): ~48 minutes in `Run JIT suites`
against a 13-19 min baseline on the two preceding `main` runs.

Filed 2026-09-28, split out of
[docs/archive/macos-jit-hang-loses-both-diagnostics.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/macos-jit-hang-loses-both-diagnostics.md)
when that report's *instrumentation* defect was fixed. This is the half that
remains, and it is deliberately thin, because the honest state is that **there
is no evidence to reason from.**

## Why there is nothing here yet

The 2026-09-28 occurrence destroyed its own diagnostics: a `timeout-minutes`
kill cancels the job and leaves every remaining step `pending`, so the
`if: always()` artifact upload never ran, and the run's log blob 404s. That is
now fixed -- see the archived report -- but it cannot be applied backwards.

Two earlier root causes are known and were genuinely fixed, so neither is a
live lead: a missing `timeout(1)` turning `httpd-async-limit`'s listen-fd
deadlock into a job kill
([docs/archive/macos-jit-leg-intermittent-45min-hang.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/macos-jit-leg-intermittent-45min-hang.md)),
and the `brew install coreutils` containment that followed it -- verified still
in place on the hung run, whose `Install dependencies (macOS)` step took 4
seconds and poured `coreutils`.

## What the next occurrence will hand you

All three now exist, so this should be a short investigation rather than
another blind one:

- **A `jit-ctest-log-macos-latest` artifact.** The step now alarms at 2100s and
  fails, which is a failing step rather than a killed job, so the upload runs.
- **The test's name.** `tur_jit_fixture_tests`, `tur_repl_spice_jit` and
  `tur_flags_tests` carry ctest `TIMEOUT` properties (1500 / 600 / 900), so a
  single hang is killed and named well inside the alarm.
- **A per-case bound inside `run-flags.sh`.** Every `$TUR` invocation there runs
  under `timeout -k 5 180`, so a hung `jit-ffi-*` case -- dynamic FFI,
  callbacks, threads, the shapes that hang -- fails as a case with its name.

## Where to look when it does

Ranked by what the fixes above cannot bound:

1. **An untimed phase around a fixture, not the fixture.** `run-jit.sh`'s
   `_run_timed` wraps the `"$TUR" ... jit "$input"` invocations; harness setup
   and any untimed compile/link is outside it. This was the archived report's
   own point 3 and is still the likeliest home for a 45-minute stall.
2. **A `jit-ffi-*` case in `run-flags.sh`.** It had no timeout of any kind until
   2026-09-28, and CI runs it in this leg precisely because it is the only
   harness whose `jit-ffi-*` cases are not skipped.
3. **Runner size.** `macos-latest` hands out 3-core and 5-core machines and CI
   draws the 3-core one ~97% of the time; `tur_jit_fixture_tests` alone is
   ~730s median / ~940s p90 there. That explains 19 minutes, not 48, so it is
   context rather than cause -- but a genuine stall plus a slow runner is what
   sets the per-test `TIMEOUT` values, and those may need revisiting if a
   legitimate run ever trips one.

## What would close this

A named stall with a cause, or a long enough quiet period on a leg that now
cannot hide one. Do not close it on the instrumentation fix alone: that made
the next occurrence legible, it did not make it stop.
