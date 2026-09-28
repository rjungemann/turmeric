# The macOS JIT leg's 45-minute hang recurred, and both diagnostics were lost

**Severity: medium.** The hang itself gates -- `JIT engine (macos-latest)` is
the one JIT leg that is not `continue-on-error`, so it fails the run. What
makes this worth its own report rather than a line on the archived one is the
second half: the instrumentation added in 2026-08-02 specifically so the next
occurrence would be diagnosable **produced nothing**, so occurrence #5 will be
as blind as #1 unless it is fixed.

Filed 2026-09-28, from rjungemann/turmeric#953.

## The occurrence

| | |
|---|---|
| Run | [36385448273](https://github.com/rjungemann/turmeric/actions/runs/36385448273), job 108809755220 |
| Step | `Run JIT suites`, started 06:14:54Z |
| End | job `cancelled` 07:09:17Z by `timeout-minutes: 45` |
| Wall | ~54 min job, ~48 min in the step |

Baseline for that step on the two immediately preceding `main` runs:
**13m and 19m** (runs 36379252305, 36381500505). So this is 2.5-3.5x the
recent worst case, not a slow runner.

It is not the PR's doing. #953 touches `web/`, `docs/`, `src/web/wasm_glue.c`
and `tests/wasm_glue_lang_unit.c`; `wasm_glue.c` links into `libturi_wasm` and
the two `tur_wasm_glue_*_unit` tests, and the step runs only
`tur_jit_fixture_tests`, `tur_repl_spice_jit` and `tur_flags_tests`. Nothing
in the diff executes there. The other 17 checks on that run passed, including
`Test (macos-latest)`.

## Both diagnostics were lost

[docs/archive/macos-jit-leg-intermittent-45min-hang.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/macos-jit-leg-intermittent-45min-hang.md)
rebuilt this step in 2026-08-02 so a hang could not be silent again: `tee
jit-ctest.log | grep --line-buffered` to stream progress to the console, and
an `Upload JIT ctest log` step with `if: always()` -- "which covers the
cancellation a timeout kill produces, so the partial log survives".

Neither survived:

- **The artifact upload never ran.** After the kill the job's steps read
  `10 Run JIT suites -- in_progress`, then `11 Upload JIT ctest log -- pending`,
  `12`, `13`, `14` all `pending`. A `timeout-minutes` kill does not run the
  remaining `if: always()` steps. The run's artifact list has
  `jit-ctest-log-ubuntu-latest` and no macOS counterpart.
- **The console log is gone too.** `GET /actions/jobs/108809755220/logs` is
  `BlobNotFound`, `gh run view --job ... --log` is `log not found`, and the
  run-level log archive contains `JIT engine (ubuntu-latest)/` and the Windows
  legs but no `JIT engine (macos-latest)` step files at all.

The archived report's "Confirmed working on run 30767617172" was a run that
**succeeded** -- so the `if: always()` claim was never exercised under the
condition it was written for. That is the defect.

## The 2026-08-18 containment is still in place

Worth stating, because it is the obvious first suspect and it is not the
cause. `ci.yml:384` still reads `brew install libedit ccache coreutils || true`,
and the hung run's `Install dependencies (macOS)` step took **4 seconds**
against **~3.5 seconds** for the successful install on run 36381500505
(`Pouring coreutils--9.11.arm64_tahoe.bottle`). So `gtimeout` was present and
per-fixture timeouts were live. `httpd-async-limit`'s own deadlock was
root-caused and fixed in the same archived report and is not implicated.

That leaves a stall somewhere a per-fixture `gtimeout` does not reach.

## Where a stall can still reach the job wall

Three structural gaps, all verified against the tree at `30ca2df47`:

1. **`tests/run-flags.sh` has no timeout wrapper at all.** `run-jit.sh` and
   `run.sh` carry the `_tur_timeout_bin` probe and `_run_timed`; `run-flags.sh`
   contains neither string. `tur_flags_tests` is in this leg deliberately (the
   CI comment: it is "the only harness carrying the `jit-ffi-*` cases", which
   are gated on the engine and skip entirely in the `test` job), and those
   cases exercise dynamic FFI, callbacks and threads -- exactly the shapes that
   hang.
2. **None of the three targets carries a ctest `TIMEOUT` property.** Verified
   with `ctest --show-only=json-v1`: `tur_flags_tests` reports `NO TIMEOUT
   PROPERTY`, and `tur_jit_fixture_tests` / `tur_repl_spice_jit` are declared
   at CMakeLists.txt:1264-1282 with `RUN_SERIAL TRUE` and nothing else. The
   same file sets explicit `TIMEOUT` on many other targets (300, 600, 720,
   900), so this is an omission rather than a policy.
3. **`_run_timed` covers the fixture invocations, not the phases around
   them.** `run-jit.sh` wraps `"$TUR" ... jit "$input"` at lines 320/324/413.
   A stall in harness setup, or in an untimed compile/link, is outside it --
   which is the archived report's own point 3 about where a 45-minute stall is
   likeliest to live.

## Fix directions

The first one is the one that matters: without it the next occurrence tells us
nothing again.

- **Bound ctest inside the job budget** so the step fails normally and the
  `if: always()` uploads get to run. The repo already has the idiom for this --
  `ci.yml:413` uses `perl -e 'alarm 30; exec @ARGV'` for the engine-present
  probe, for exactly the reason that stock macOS has no `timeout(1)`. Wrapping
  the ctest call in `perl -e 'alarm 2100; exec @ARGV'` leaves ~10 minutes of
  the 45 for the artifact upload.
- **Give the three targets explicit ctest `TIMEOUT` properties**, in the style
  the rest of CMakeLists.txt already uses. A per-test kill also makes ctest
  name the test that died, which is most of the diagnosis.
- **Add the `_run_timed` probe to `tests/run-flags.sh`**, so its fixtures fail
  at a per-case timeout like every other harness's.
- Then, with a log in hand, find the actual stall. Until one of the above
  lands, do not spend time guessing which of the three targets it was -- the
  evidence for this occurrence no longer exists.

## Note on the archive

The archived report's root cause (a missing `timeout(1)` turning
`httpd-async-limit`'s listen-fd deadlock into a job kill) was genuinely fixed,
and its fixes are still in the tree -- so it stays archived rather than
returning here. This report is the different defect wearing the same symptom,
and the archive carries a forward pointer to it.
