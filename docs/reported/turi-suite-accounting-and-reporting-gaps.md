# `turi_fixture_tests` counts 2615 of 2714 fixtures, and none of the count reaches CI

**Severity:** medium. Nothing here makes a passing fixture fail or a failing one
pass. What it does is remove the suite's ability to notice that it stopped
running things -- which is the same failure mode as
[fixture-dirs-with-loose-tur-files-pass-without-running](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md)
(resolved 2026-08-05, on `run.sh`) and KB-001 (`docs/archive/history/known-bugs.md`,
on this same harness), wearing a third face.

**Status:** OPEN. Filed 2026-08-29 from a question about whether
`turi_fixture_tests` should be split into its own ctest target. It should not --
**it already is one** (`CMakeLists.txt:833-841`, `RUN_SERIAL`, entirely separate
from `tur_tests`). What prompted the question is the reporting, and the
reporting has six defects.

## Measured baseline

`TUR_FORCE=1 bash tests/run-turi.sh`, local, Debug, 8 cores, sibling
`../turmeric-spices` present:

```
turi fixture summary: 1854 passed, 31 failed, 737 skipped
  (of which 737 inline-c carve-outs -- TI7, never run under turi)
```

Against the tree the harness actually walks:

| | count |
| --- | --- |
| positive fixture dirs with an input file | 2191 |
| `tests/fixtures/errors/*` with an `input.tur` | 523 |
| **discovered total** | **2714** |
| counted in the summary (1854 + 31 + 737, less the 7 non-fixture async scripts) | 2615 |
| **never counted at all** | **99** |

The 31 FAILs are real and pre-existing (a `String`/`show`/`rational` cluster
plus the `sx1`/`sx2` trail fixtures); they are not this report's subject and
are not analyzed here.

## 1. Marker skips write no result file -- 81 fixtures

`run_turi_fixture` prints a `SKIP` line and returns *without writing a
`$RESULTS_DIR/<name>.result`* for all five marker skips
(`tests/run-turi.sh:275-284`). Only the inline-C carve-out writes one
(`:300`). The tally at `:444` iterates `*.result`, so a fixture with no result
file is in no bucket -- not PASS, not FAIL, not SKIP.

Observed: **818 `SKIP` lines printed, 737 counted.** The 81-line gap breaks
down as 53 `requires.compiled`, 18 `requires.tsan`, 9
`requires.dedicated-runner`, 1 `requires.tur-only`. (`requires.spices`
contributed 0 here because the sibling checkout is present; in CI, where it
usually is not, it contributes too.)

**Fix direction.** Write a result file on each of the five paths. `run.sh`
already solved this the other way -- it records a skip as `PASS` with a
`(dedicated-runner-skipped)` detail (`tests/run.sh:524-567`), which keeps its
total stable. Either convention works; the requirement is that every
discovered fixture lands in exactly one bucket.

## 2. The error pass returns silently -- 18 fixtures, no output at all

Worse than item 1, because these print nothing whatsoever.
`run_turi_error_fixture` bails with a bare `return` for a fixture with no or
empty `expected.diag` (`tests/run-turi.sh:220`) and for the marker skips
(`:221-222`). Measured: 505 of the 523 discovered error fixtures were
accounted for; the missing 18 are 11 marker skips and 7 with a missing or
empty `expected.diag`.

The 7 are the interesting ones. An `errors/` fixture with no `expected.diag`
asserts nothing under this harness and says nothing about it -- which is
exactly the shape of the loose-`.tur`-files bug, in the negative-fixture pass
that bug's fix did not cover.

**Fix direction.** Same as item 1 for the marker paths. For the empty-diag
case, print and record it -- and consider whether it should be *loud*, as the
`run.sh` fix made its equivalent: an `errors/` directory that carries an input
and no expected diagnostic is more likely a mistake than a decision.

## 3. The error pass honours a different marker set than the positive pass

`run_turi_error_fixture` checks `requires.compiled`, `requires.tur-only` and
`requires.spices` only (`:221-222`). The positive pass additionally honours
`requires.dedicated-runner` and `requires.tsan` (`:280-284`). So an `errors/`
fixture carrying `requires.dedicated-runner` is run here anyway -- there is
one in the tree today. And `requires.spices` is checked *unconditionally* on
the error path, where the positive path checks it only when the sibling
checkout is absent (`:282`), so one error fixture skips on a machine that
could run it.

**Fix direction.** One shared marker-skip helper called by both passes.

## 4. The errors denylist names a fixture that does not exist

`TURI_ERRORS_DENY` (`tests/run-turi.sh:194`) has exactly one entry,
`defdata-malformed-ctor-field-type`, carried with a 12-line comment explaining
the interpreter's cascade-diagnostic divergence. **There is no
`tests/fixtures/errors/defdata-malformed-ctor-field-type/`.** The denylist
matches nothing; the run printed zero `SKIP errors/` lines.

So either the fixture was renamed or deleted and the carve-out is dead prose,
or the divergence it documents is now uncovered. The comment is the only
surviving record of a real interpreter/compiler difference, which is worth
keeping somewhere even if the entry goes.

**Fix direction.** Establish which happened (`git log -- tests/fixtures/errors/`),
then delete the entry or repoint it. Separately: a denylist entry that matches
no fixture should be a hard error at harness startup -- it costs three lines
and this is the second stale-carve-out finding in this file.

## 5. The 7 `eval-async-*.sh` scripts run twice per CI job

`tests/run-turi.sh:460-471` runs every `tests/turi/eval-async-*.sh` and folds
the result into its own PASS/FAIL counts. All seven are *already* registered as
their own ctest targets -- `tur_eval_async_basic` ... `tur_eval_async_io`,
`CMakeLists.txt:1177-1211`. Every CI job runs each of them twice, and turi's
reported PASS count is inflated by 7 with things that are not fixtures.

**Fix direction.** Delete the block. The targets already exist, already carry
their own `detect_leaks=0` opt-out, and already report individually -- which is
strictly better than being averaged into a 2600-fixture number.

## 6. None of the count reaches CI

On a green run `turi_fixture_tests` prints **zero** lines to the CI console.
The auxiliary-suite step uses `--output-on-failure`
(`.github/workflows/ci.yml:179`), so the summary at `tests/run-turi.sh:474`
never surfaces. The single user-facing artifact is ctest's own
`Test #NN: turi_fixture_tests ... Passed  100.9 sec`.

The summary *is* captured -- ctest embeds it in the JUnit `<system-out>` -- but
nothing reads it. `tools/ci/collect-suite-timings.py` greps that text for
`TUR_SKIP:` / `TUR_SKIP_PARTIAL:` and discards the rest, and
`build/results-aux.xml` is never uploaded (only the derived `timings.jsonl`
is). So the pass/fail/skip counts of the largest interpreter suite in the
project exist for about 200 milliseconds and are then thrown away.

Compounding it: **`run-turi.sh` emits no `TUR_SKIP_PARTIAL:` marker**, though
it is the strongest candidate in the tree for one -- it permanently skips 737
fixtures (27% of what it discovers) to the inline-C carve-out. The timing
plan's stated secondary goal is "detect suites that silently start skipping"
(`docs/upcoming/suite-timing-trends-plan.md`); this suite skips more than a
quarter of its corpus and the ingest records `status: "pass"` with no note.

**Fix direction**, cheapest first:

1. `echo "TUR_SKIP_PARTIAL: inline-c carve-out (N fixtures)"` from the summary
   block. One line, and the existing ingest already understands it.
2. Once items 1-2 make the summary a census, print the denominator:
   `turi fixture summary: P passed, F failed, S skipped of D discovered`.
3. Teach `collect-suite-timings.py` to parse `passed/failed/skipped` out of
   `<system-out>` into the row. That turns the census into a trend, which is
   what detects a silent drop; the summary line alone only detects one if a
   human is reading a log nobody prints.

Per-fixture drill-down is Phase 6 of the timing plan, explicitly gated on
evidence not yet gathered. Nothing here needs it.

## Not a defect: the suite does not need splitting

Stated because it was the original question. `turi_fixture_tests` is its own
`RUN_SERIAL` ctest target and has been. It looks subordinate in CI only
because `ci.yml:155` gives `tur_tests` a dedicated step while `:179` sweeps
~105 other targets, this one included, into a single `-E '^tur_tests$'`
invocation.

Splitting the positive and error passes into two targets would buy two console
lines and two timing series at the cost of two `RUN_SERIAL` scheduling slots,
and would separate halves of one parity story. The reporting gaps above are
the actual cost, and none of them is fixed by splitting. The one thing that
*should* leave the suite is the async block (item 5), which is not a fixture
pass at all.

## Guides to update when fixed

- docs/guides/test-suite-portability-guide.md -- Section 7a ("Leak checking -- what is
  covered and what is not") is the only place in the guides that spells out
  what a suite does *not* cover. The 737-fixture inline-C carve-out and the
  marker skips deserve the same treatment; a reader currently has to derive
  both from the harness source.
- `CLAUDE.md`'s `requires.*` table documents `requires.interp-only` and
  `requires.interp` carefully but says nothing about which markers
  `run-turi.sh` honours, which items 1 and 3 show is not the same set
  `run.sh` honours.
