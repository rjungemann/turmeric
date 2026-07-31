# macOS CI fails both fixture suites while Linux passes on the same commit

**Severity: medium, undiagnosed.** On PR #752 the two macOS CI jobs fail while
their Linux counterparts pass on the identical SHA. Not reproducible from a
Linux container, so this needs someone with a macOS box (or a CI re-run with
more logging) to take further.

Found 2026-07-31 while triaging CI on PR #752.

## What fails

Run 2197, head `c091889a4`
(https://github.com/rjungemann/turmeric/actions/runs/30621915696):

| job | ctest target | result |
| --- | --- | --- |
| Test (macos-latest) | `tur_tests` | **FAILED** (0% of 1) |
| JIT engine (macos-latest) | `tur_jit_fixture_tests` | **FAILED** (50% of 2) |
| Test (ubuntu-latest) | `tur_tests` | passed |
| Test (ubuntu-latest) | `turi_fixture_tests` | failed -- unrelated, see below |
| JIT engine (ubuntu-latest) | `tur_jit_fixture_tests` | **passed** |

So the compiled suite AND the JIT suite both fail on macOS and both pass on
Linux, same commit, same fixtures.

The ubuntu `turi_fixture_tests` failure is a different thing and is filed
separately (`ci-cps-tramp-turi-timeouts-under-load.md` plus the real
`turi-hkt-constrained-byvalue-bind-pure-wrong-values.md`).

## Not caused by the PR's recent commits

The previous CI run on this branch (2196, head `dbfecd0d2`, before the last two
commits) shows the same two macOS jobs already failing. In that run
`Test (macos-latest)` did not even fail cleanly -- the fixture-suite step ran
from 08:56 to 14:55 (**six hours**) and was cancelled, which is itself a
timeout/hang signature rather than a fixture mismatch.

`Check codegen snapshots` failed in 2196 and passes in 2197, so the snapshot
regen in `c091889a4` fixed that one; it is not related to the macOS failures.

## Local evidence (Linux, for contrast)

At the same commit, on a 4-core Linux box:

```
tests/run.sh                                   2499 passed, 0 failed
tests/run-jit.sh (Release, -DTUR_JIT=ON)       2414 passed, 0 failed, 47 skipped
```

The macOS agent's own commit message on `dbfecd0d2` likewise reports
`JIT: 2414 passed, 0 failed` and `AOT: 2495 passed, 4 failed` measured
locally on macOS -- which disagrees with what macOS CI reports for the same
tree. That gap is the most interesting part of this report: a local macOS run
and a macOS CI run of the same suites do not agree.

## Leading hypothesis (unverified)

CLAUDE.md documents two macOS-specific build traps that would produce exactly
this shape, and both differ between a hand-run local build and CI:

- **Sanitizer/toolchain mismatch.** `TUR_DEBUG_SANITIZE` defaults ON
  everywhere including macOS, and a local macOS build commonly turns it off
  (or uses Homebrew LLVM) to dodge the ASan startup deadlock. A local run with
  sanitizers off and a CI run with them on are not the same experiment -- and
  this session already saw a latent heap-use-after-free
  (`emit_module.c` ret_ctype, fixed in `c091889a4`) that was invisible without
  ASan and failed 43 fixtures with it.
- **Timeouts under CI contention.** The six-hour hang in run 2196 points at
  something that does not terminate rather than something that mismatches.

## What was not determined

The failing fixture NAMES were not extracted. The Actions log API returns a
tail window, and for both macOS jobs the tail is occupied by the harness's
skip list and the ctest summary; the per-fixture `FAIL` lines scroll off above
it. Getting them needs either a larger log fetch, a re-run with the harness
output uploaded as an artifact, or a local macOS run.

## Fix directions

1. Get the fixture names first -- everything else is speculation until then.
   Easiest path is uploading `tests/**/actual.*` (or the harness stdout) as a
   CI artifact on failure.
2. Reconcile the local-macOS vs macOS-CI disagreement explicitly: run the
   suites on macOS with `TUR_DEBUG_SANITIZE=ON` and the same generator CI
   uses, and see whether the local numbers move to match CI.
3. If it turns out to be the six-hour-hang class rather than mismatches, treat
   it as a hang hunt (untimed compile phase) rather than a fixture triage --
   CLAUDE.md's note that a runtime loop surfaces as a FAIL, not a stall, means
   an indefinite stall is in `emit-c`/`build`.
