# `JIT engine (ubuntu-latest)` fails 18% of commits, and `continue-on-error` absorbs all of it

**Severity: medium.** No known product defect -- a *signal* defect. The leg
fails on roughly one commit in five and nothing goes red, so nobody is reading
it, and a real JIT regression landing on Linux would look exactly like the
existing noise.

Filed 2026-10-03 while diagnosing the macOS leg's intermittent red
([macos-jit-leg-stall-unexplained](macos-jit-leg-stall-unexplained.md)). The
macOS number turned out to be 1%; measuring the Linux one to compare is how
this surfaced.

## The measurement

One entry per commit, from `suite-timings-2026.jsonl` on the `ci-metrics`
branch, suite `tur_jit_fixture_tests`:

| env | commits | pass | fail | fail rate | episodes |
| --- | --- | --- | --- | --- | --- |
| Linux GNU-13.3.0, 4 cores | 302 | 247 | 55 | **18%** | 37 (29 isolated, longest 10) |
| macOS AppleClang-21, 3 cores | 289 | 287 | 2 | 1% | 2, both isolated |

Duration is not the story here the way it is on macOS: Linux runs
min 267 / p50 501 / p90 630 / max 738 s, a far tighter spread than macOS's
500-1648 s, and well inside the suite bound.

`ci.yml` makes the asymmetry deliberate -- `continue-on-error: ${{ matrix.os
!= 'macos-latest' }}`, with a comment calling it "BLOCKING on macOS,
non-blocking on Linux". The intent was that macOS is the fragile leg. The data
says the opposite: Linux fails 18x more often, and it is the leg that cannot
fail the run.

## Why this is worth a report rather than a shrug

The project has just been through this exact shape twice on the browser
suites. A suite that is always a bit red teaches readers to ignore it, and the
gap is then invisible rather than merely tolerated:

- `docs/archive/try-turmeric-browser-suites-green-while-failing.md` -- five
  failures inside a green job, with the report that would explain them
  discarded by an `if: failure()` upload that `continue-on-error` made
  unreachable.
- `docs/reported/docs-offline-cold-pane-never-boots.md` -- one test failing on
  every run for an unknown length of time, which held the desktop suite at
  `1 failed` for 21 consecutive commits and made a genuinely new regression
  indistinguishable from the usual.

Both were real defects that non-gating had hidden. 18% is a larger hiding
place than either.

## What is NOT established

**Which fixtures fail, and whether it is one cause or many.** This report is
the measurement only. The `/ci` rows carry per-suite status, not per-fixture
names, so identifying the failures means reading
`jit-ctest-log-ubuntu-latest` artifacts across several red commits. That has
not been done, and nothing here should be read as implying the failures are
flake: 29 of 37 episodes are isolated, but one ran 10 commits long, which is
a standing-defect shape.

## Fix directions

1. **Name the failures first.** Pull `jit-ctest-log-ubuntu-latest` from a
   handful of red Linux commits and group by fixture. If one fixture
   dominates, this is a specific bug; if it varies, it is resource or
   ordering. Until that is known, any gating change is premature.
2. **Then reconsider the asymmetry.** The comment in `ci.yml` justifies
   non-gating on Linux by fragility that the data locates on the other leg.
   Whichever way it is resolved, the comment should describe the measured
   state rather than the assumed one.
3. **Do not simply flip the gate.** At 18% that would block most PRs. The
   browser-suite precedent is the order to follow: establish a clean baseline
   (fix or mark the standing failures) and only then gate, so the failure
   count means "something new broke".
4. `run-jit.sh` now prints the fixtures closest to their own timeout budget,
   which will separate budget-marginal failures from real ones at a glance.

## How this was measured

`git show origin/ci-metrics:suite-timings-2026.jsonl`, filtered to
`suite == "tur_jit_fixture_tests"`, deduplicated by `sha`, grouped by the
`(os, cc, nproc)` build-shape tuple, with consecutive same-status runs
collapsed to distinguish isolated failures from episodes. No CI logs were read
and no fixture was run, which is exactly the limit recorded above.
