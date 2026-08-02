# macOS JIT CI leg intermittently hangs to its 45-minute timeout

**Severity: medium.** Intermittent, not reproduced locally, and it gates: the
`JIT engine (macos-latest)` leg is the one JIT job that is not
`continue-on-error`, so a hang fails the run. Two occurrences within 40 minutes
on 2026-08-02, one of them on `main`.

## Timeline

`JIT engine (macos-latest)`, from `gh run list --branch main`:

| run | started -> completed | result |
|---|---|---|
| 30728085832 | 02:08:05 -> 02:18:35 | success (10m30s) |
| 30740318014 | 08:43:06 -> 08:51:55 | success (8m49s) |
| 30740496389 | 08:48:31 -> 08:57:50 | success (9m19s) |
| 30740626181 | 08:58:53 -> 09:07:26 | success (8m33s) |
| 30741777438 | 09:26:42 -> 09:36:41 | success (9m59s) |
| **30764326918** (`main`, f865097db) | **19:52:15 -> 20:37:29** | **HANG -- 45m14s, killed by `timeout-minutes: 45`** |
| **30764785764** (`claude/ecs-write-frames`) | **20:30:47 -> 21:15:xx** | **HANG -- same 45m wall** |
| 30767617172 (same branch, next push) | 21:19:23 -> 21:29:59 | success (10m36s) |

So: five consecutive ~9-10 minute successes, two 45-minute hangs, then a clean
10m36s run on the same branch that had just hung. **Intermittent.**

Both hangs were inside the `Run JIT suites` step. Neither produced a single
line of output -- see below.

## It is not a branch-side regression

The `claude/ecs-write-frames` hang was initially suspected to be that branch's
turi change (`turi_copy_byvalue_struct_arg`). It is not:

- `main`'s leg hung at 19:52, and the branch's turi commit was not pushed until
  20:04. `main` hung first, independently.
- The same branch passed at 21:19 in 10m36s with that commit still in place.
- The JIT leg exercises the compiled/JIT path; the turi change only affects
  `--interpret`.

## Why the first two hangs produced no diagnostic at all

The step used to be:

```bash
ctest --test-dir build -V --no-tests=error -R '...' > jit-ctest.log 2>&1
rc=$?
grep -aE '...' jit-ctest.log      # only after ctest exits
```

When the job hits `timeout-minutes`, ctest is killed, so the grep never runs
and the console gets nothing; `jit-ctest.log` then dies with the runner. The
only recoverable fact about either hang was "the step named `Run JIT suites`
was `in_progress`".

**Fixed 2026-08-02** (same commit as this report's filing): the step now pipes
through `tee jit-ctest.log | grep --line-buffered`, so filtered lines stream as
they happen, and the log is uploaded with `if: always()` -- which covers the
cancellation a timeout kill produces, so the partial log survives. `Start <n>:`
lines joined the filter so the console names which ctest test is stuck.

Confirmed working on run 30767617172: the console now carries timestamped
progress, e.g.

```
21:19:53   Start 55: tur_jit_fixture_tests
21:19:59  55: PASS arc-basic (via cc fallback)
21:20:21  55: PASS cancel-select (via cc fallback)
21:22:30  55: PASS future-linear (via cc fallback)
```

## What to do on the next occurrence

The instrumentation exists now, so the next hang should be diagnosable without
guessing:

1. Read the **last timestamped line** in the `Run JIT suites` step -- it names
   the fixture the harness stalled after.
2. Download the `jit-ctest-log-macos-latest` artifact for the full `-V` log.
3. Note that a per-fixture timeout would normally surface a runtime hang as a
   FAIL, so a stall that reaches 45 minutes is more likely in an *untimed*
   compile/link phase, or in the harness's own setup, than in a fixture's run
   phase. The `(via cc fallback)` fixtures are the ones that shell out to `cc`
   and are the natural first suspects on a macOS box.

## ROOT CAUSE (found 2026-08-02, third occurrence)

The third hang was the first one with the streamed log and artifact in place,
and they answered it. Diffing the killed run's fixture set against a successful
run's:

- 2484 fixtures in the good run, 2046 in the hung one.
- The 438 missing are **all 437 `errors/*` fixtures** -- the hung run reached
  *zero* of them -- **plus exactly one other: `httpd-async-limit`.**

So the harness finished every non-`errors/` fixture except `httpd-async-limit`,
stalled there, and never started the `errors/` phase.

`httpd-async-limit` is a TCP networking fixture: it starts the async HTTP
server with `max-in-flight=2` and a 100ms slow handler, spawns four client
threads that open real sockets, and asserts 2 succeed + 2 get a 503. A socket
accept/connect race or a port-bind conflict hanging intermittently is entirely
ordinary for that shape.

**What turns a flaky fixture into a 45-minute job kill is the missing timeout
binary.** `tests/run-jit.sh` (and `tests/run.sh`) detect `timeout`, then
`gtimeout`, and run **UNTIMED** when neither exists:

```sh
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then _tur_timeout_bin="gtimeout"; fi
_run_timed() {
    local secs="$1"; shift
    if [ "$secs" -le 0 ] || [ -z "$_tur_timeout_bin" ]; then "$@"
    else "$_tur_timeout_bin" "$secs" "$@"; fi
}
```

Their own comment names the consequence: *"run untimed if neither exists (a
hung fixture then hangs the run, which is the pre-existing tradeoff run.sh
already makes)"*. Stock macOS ships no `timeout(1)`, `gtimeout` comes from
Homebrew coreutils, and the macOS CI steps installed only `libedit ccache`.
The repo already knew macOS lacks it -- the `test` job's smoke check uses
`perl -e 'alarm 10'` for exactly this reason -- but the fixture harnesses were
left on the untimed path.

That explains every observation: macOS-only (Linux has `timeout`), intermittent
(the fixture is a flaky network race), always exactly 45 minutes (the JOB
timeout, since no fixture timeout ever fires), and a hang rather than a FAIL.
It also explains why `Test (macos-latest)` began hanging in the same window --
same harness, same missing binary.

**Fixed** by adding `coreutils` to both macOS dependency steps. That is
containment, not a cure: with `gtimeout` present the fixture dies at its
per-fixture timeout and reports a FAIL you can act on, instead of eating the
job.

## Still open: why `httpd-async-limit` hangs at all

The flakiness itself is unfixed and is the real bug. It passes far more often
than it hangs (it is green in the two successful macOS runs above), so it wants
a loop-until-it-reproduces run rather than a single repro attempt. Suspects, in
order: the accept loop when in-flight is already at the cap (the 503 path is
the fixture's whole point and the least-exercised branch), a client thread
blocking in `connect()` against a listen backlog, and port reuse across
concurrent fixtures on the same runner.
