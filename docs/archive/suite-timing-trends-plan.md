# Plan: Test Suite Timing Trends for `rjungemann/turmeric`

> **Archived 2026-09-09.** Phases 0-5 landed (the `/ci` dashboard shipped
> 2026-08-28) and Phase 6 was declined by the two-week readout below:
> `tur_tests` is flat, so there is nothing for a per-fixture drill-down to
> explain. The pipeline defect the readout found (ctest truncating the
> census line of the two largest suites) is fixed in `ci.yml`; the coverage
> holes it found are filed as
> [ci-suites-that-never-run-on-hosted-runners](../reported/ci-suites-that-never-run-on-hosted-runners.md).
> The `ci-metrics` branch README still names this plan's old path; it is
> only ever written on the branch's first publish, so it is stale by one
> directory and harmless.

## Execution record (2026-08-25; two-week readout added 2026-09-09)

**Phases 0-5 are implemented and validated; Phase 6 is declined by the data.**
Phase 5 (the dashboard) shipped 2026-08-28 as `/ci` -- `web/ci/index.html`
and `web/ci-metrics.js`, with `web/worker.js` proxying the `ci-metrics`
branch as `/api/ci-timings` and `web/tests/ci.spec.js` covering the page --
carrying all four views listed under Phase 5 (duration over time, per-suite
sparklines, the suite table, the skip ledger). This header said "deliberately
NOT done" for twelve days after that. Phase 6 was gated on "only if
`tur_tests` turns out to be the thing that moves"; the readout below shows it
did not move. Nothing in this plan is open.

Landed:

- `tools/ci/collect-suite-timings.py` -- merges N JUnit files, emits NDJSON.
- `tools/ci/publish-timings.sh` -- appends to the `ci-metrics` orphan branch.
- `.github/workflows/ci.yml` -- `--output-junit` on all three ctest
  invocations, collect+upload on both `test` and `jit`, and a new
  `publish-timings` job.
- 14 `TUR_SKIP:` / `TUR_SKIP_PARTIAL:` markers across 10 harnesses.

### What the plan got wrong (corrected in the implementation)

1. **A relative `--output-junit` path resolves against `--test-dir`, not the
   working directory.** `ctest --test-dir build --output-junit results-aux.xml`
   writes `build/results-aux.xml`. The plan's Phase 2 reads the files from the
   workspace root, which yields zero rows -- and because the collect step only
   warns, it would have shipped as a silently empty dashboard. The workflow now
   passes `build/results-*.xml` explicitly, with a comment.

2. **JUnit already carries per-test stdout.** Phase 1 asserted "JUnit output
   alone will not carry this ... either add `-V`/`--verbose` or capture via
   `tee`." False: ctest embeds each testcase's output in `<system-out>` even
   under `--output-on-failure`. No `-V`, no `tee`, no `LastTest.log`. This
   removes the plan's only real objection to the skip-marker design and keeps
   the console output unchanged.

3. **The skip inventory was over-counted: 10 harnesses, not 13.** The Phase 1
   table came from a loose heuristic (`command -v` near an `exit 0`) and
   carried five false positives. `run-bench.sh`'s `command -v clock_gettime` is
   a timing-mechanism fallback that never exits early; `run-turi.sh`'s `SKIP`s
   are per-fixture; and `run-build-project.sh`, `run-flags.sh`, and
   `run-install.sh` have no whole-suite skip at all (their `echo "PASS $1"` hits
   are the generic `pass()` helper, not a skip site). Conversely `run-jit.sh`
   has a **fourth** convention the table missed: `run-jit: SKIP (...)`.

4. **`tur_phase4_gdb` / `tur_phase5_gdb` are PARTIAL skips, and the plan's
   acceptance criterion for them was wrong.** It asked that `tur_phase4_gdb`
   "land as `skip`". It must not: only the gdb half is skipped: the emit-c
   assertions still run, and the suite still takes real time (586 ms and
   1392 ms measured). Recording it as a skip would drop a working suite from
   the duration trend -- the exact failure mode Phase 1 exists to prevent, in
   the opposite direction. These two now emit `TUR_SKIP_PARTIAL:`, which the
   ingest records as `status: "pass"` plus a `partial_skip_reason` note.

5. **Force-push replaced with fetch/append/retry.** Phase 3A specified a
   force-push. That can silently discard rows another run appended between our
   fetch and our push -- the one outcome an append-only metrics log must never
   have. The script now re-fetches, re-appends onto the new tip, and retries
   (5 attempts, backoff). Verified: with a second publisher's row already on the
   remote, publishing preserved both.

6. **Publishing is a separate job, not a step.** Phase 4 put
   `concurrency: {group: ci-metrics}` on the canonical job. Hung on `test` that
   would serialize the entire build-and-test matrix across runs -- an enormous
   cost for a metrics feature. The `publish-timings` job carries the group
   instead, so it serializes only an artifact download and a git push.

7. `continue-on-error` was not used, per the plan's own Phase 4 note; the
   collect/upload steps are `if: always()`, leaving the gate untouched.

### CI never runs on `ci-metrics`

Two independent layers: `ci.yml` triggers only on `branches: [main]` (documented
in the `on:` block so it is not widened casually), and every publish commit
carries `[skip ci]`. `release.yml` triggers on `v*` tags only. The branch is a
true orphan (no parent, no source files -- verified) carrying only a README and
`suite-timings-<year>.jsonl`.

### Measured baseline (local, Debug, AppleClang-21.0.0, 8 cores)

111 auxiliary suites ingested cleanly: 109 pass, 2 fail, 2 partial-skips. The
two failures (`tur_engine_select`, `turi_fixture_tests`) are **pre-existing** --
`tur_engine_select` reproduces with all of this work stashed. Slowest suites:
`tur_span_coverage` 264.8 s, `turi_fixture_tests` 100.9 s,
`tur_spice_resolver_tests` 39.7 s. Note `tur_span_coverage`, not `tur_tests`,
dominates the auxiliary run -- worth knowing before Phase 6 assumes `tur_tests`
is the thing to drill into.

### Two-week readout (2026-09-09)

Read from the `ci-metrics` branch at commit `95fddfc0`: 22,620 rows from 89
`main` pushes between 2026-08-26 and 2026-09-09, in five environments.

| environment (os, build, cc, nproc, jit) | runs | suites |
|---|---:|---:|
| Linux, Debug, GNU-13.3.0, 4, jit=false (canonical) | 88 | 144 |
| macOS, Debug, AppleClang-21.0.0, 3, jit=false | 89 | 147 |
| Linux and macOS, jit=true (the `jit` job) | 88 / 89 | 3 |
| Linux, emscripten (the browser suites, since 2026-09-03) | 50 | 2 |

The canonical suite count grew from 115 to 144 over the window; 29 suites
registered mid-window, which the dashboard handles by absence, per Phase 2.
Every figure below is the canonical environment, comparing the median of the
first half of the window (to 2026-09-04) against the second half.

**Phase 6 verdict: `tur_tests` did not move.** 474.8 s to 472.6 s (-0.5%).
Its series is bimodal -- roughly 300-380 s on some runs and 460-500 s on
others, with no correlation to the commit under test -- which is runner
hardware, not code. A per-fixture drill-down would rank fixtures inside a
suite whose total is flat, so Phase 6 is declined rather than deferred.

**What did move.** The sum of per-suite medians went from 1259 s to 1344 s,
and all of the delta is in these rows:

| suite | before | after | attribution |
|---|---:|---:|---|
| `tur_examples_check` | 12.1 s | 101.5 s | a step at `301f4af8` (2026-09-03), the merge that carried the guestbook-as-a-spice rewrite (`37e5670f`) and the examples ratchets (`9b6f08b2`); `examples/` went from 17 to 20 `.tur` files, so an 8x sweep cost is a per-file cost question for `tests/check-examples.sh`, not a corpus-size one |
| `tur_sr2_seam` | 7.8 s | 35.7 s | retired 2026-08-27, restored 2026-09-04 (`f2798113`) as the OFF-path gate compiling the carrier path; expected |
| `tur_leak_check` | 24.4 s | 35.2 s | `requires.leak-check` opt-ins went from 60 to 99 fixtures over the window; growth by design |
| `tur_build_project` | 37.3 s | 14.3 s | dropped at the same `301f4af8` merge |
| `tur_span_coverage` | 227.3 s | 234.7 s | +3%, noise-level, but the second-largest suite and the one Phase 6 would have had to look at next |

**A pipeline defect the readout exposed, fixed 2026-09-09.** Not one
`tur_tests` or `turi_fixture_tests` row in the 22,620 carries the
`passed` / `failed` / `skipped` / `discovered` census the ingest parses, and
turi's `TUR_SKIP_PARTIAL: inline-c carve-out` marker never reached the data
either -- the very counts `turi-suite-accounting-and-reporting-gaps` item 6
wanted as a trend. Cause: ctest keeps only the first 1 KiB of a passing
test's output (`--test-output-size-passed` defaults to 1024, and the default
`--test-output-truncation tail` drops the END), so any harness that prints
more than 1 KiB of `PASS` lines loses its trailing summary before it reaches
`<system-out>`. Reproduced with a 3000-line synthetic test on CMake 3.28
(the output is replaced by "[... was removed since it exceeds the threshold
of 1024 bytes.]") and fixed with `--test-output-size-passed 262144
--test-output-truncation head` on all three ctest invocations in `ci.yml`;
the same synthetic test then yields `passed=3000` through the ingest. Rows
before 2026-09-09 stay countless; the census series starts at the next
`main` push. Phase 1's claim above that "JUnit already carries per-test
stdout" was true only for suites that print less than a kilobyte.

**The Open question, answered from the skip ledger.** Four suites have never
run in full on a hosted runner, and the ledger is the first place that said
so:

- `tur_phase4_gdb` and `tur_phase5_gdb`: `TUR_SKIP_PARTIAL: gdb unavailable`
  on every run, on BOTH operating systems. The native-backtrace and the
  DWARF / pretty-printer halves of the debugger tests have never executed in
  CI.
- `tur_tutorial_steps`: skipped on every macOS run (`pyyaml unavailable`);
  runs on Linux.
- `tur_refine_wasm`: `emcc not on PATH` on every run of the `test` job, both
  OSes (only the browser job sets up Emscripten).
- `tur_scscm_compile`: the sibling `turmeric-spices` checkout is absent on
  every run, both OSes.

Filed as
[ci-suites-that-never-run-on-hosted-runners](../reported/ci-suites-that-never-run-on-hosted-runners.md),
which is what the question said such a finding deserves.

**Failure ledger.** "Flake detection" is a non-goal above, but the rows
already say this much:

- `tur_jit_fixture_tests` on the Linux `jit` leg failed on 11 of 88 runs,
  invisible because that leg is `continue-on-error`. The latest (`73f6ca81`)
  is `httpd-h6-routing -- stdout mismatch`, 2805 passed / 1 failed / 59
  skipped / 29 via the cc fallback. The "FLIP LINUX TO BLOCKING" note in
  `ci.yml` says its precondition is met; a 1-in-8 red rate says the flip
  would block one `main` push in eight, and
  [ci-two-fixtures-flake-on-hosted-runners](../reported/ci-two-fixtures-flake-on-hosted-runners.md)
  already has the httpd family flaking under JIT.
- `web_mobile` failed on 49 of the 50 runs since it began reporting on
  2026-09-03 (WebKit; tracked in
  [webkit-sw-controlled-reload-fails-wasm-init](../reported/webkit-sw-controlled-reload-fails-wasm-init.md)).
  `web_desktop` passed 47 of 50.
- `tur_reported_index_lint` failed twice (2026-08-28, 2026-09-01); index
  drift, fixed by the next push each time.
- `0dca9811` (2026-09-09 17:28 UTC, the newest `main` push in the data):
  every `ubuntu-latest` job died in `apt-get update` with a
  `Hash Sum mismatch` on the runner image's Google Chrome apt source
  (`dl.google.com/linux/chrome-stable`), before any step of ours ran. The run
  has no Linux rows at all, and the browser suites report
  `suite did not run (no JUnit output)`. Not this repo's packages; the seven
  `apt-get update` lines in `ci.yml` share the exposure, and dropping that
  source before updating (or retrying the update) would close it.

## Goal

Track wall-clock duration of each CTest suite over time, so duration
regressions are visible per-suite rather than as one opaque CI number.
Secondary goal: detect suites that silently start skipping.

## Context / constraints

*Verified against the tree 2026-08-25; the numbers below replace the estimates
this plan was drafted with.*

- **116 `add_test(` calls** in `CMakeLists.txt`, of which **112 register in a
  default Debug build** (`ctest --test-dir build -N`). The rest are conditional
  (`TUR_JIT`, `NOT WIN32`). Each `add_test` entry is the natural "suite"
  boundary. Most wrap a `tests/run-*.sh` harness; a handful are compiled unit
  binaries (`tur_refine_solver` #52, `tur_eval_basic` #74).
- **Seven suites are `RUN_SERIAL`**, not two: `tur_tests`, `tur_leak_check`,
  `tur_repl_spice_reload`, `tur_repl_spice_watch`, `tur_repl_spice_jit`,
  `tur_jit_fixture_tests`, `turi_fixture_tests` (`CMakeLists.txt:136,150,721,
  734,755,768,792`). `tur_tests` and `turi_fixture_tests` additionally fan out
  across `nproc` internally.
- Debug builds carry ASan+UBSan by default (`TUR_DEBUG_SANITIZE=ON`, confirmed
  in `build/CMakeCache.txt`).
- Several suites skip cleanly on a missing tool and exit 0 -- **at least 12**,
  not the 5 originally listed, and they already print markers in three
  incompatible formats. See Phase 1, which changes substantially as a result.
- **CI invokes `ctest` twice per job**, not once. This is the single biggest
  correction to the plan; see Phase 0 and Phase 4.

## Dimensions (must be recorded on every row)

Timings are only comparable within a fixed tuple of:

- `build_type` -- Debug vs Release differ by an order of magnitude
- `os` / runner label
- `cc` -- compiler id + version
- `nproc` -- the self-parallelizing harnesses scale with it
- `jit` -- whether `TUR_JIT` was ON (changes which suites exist)

Never plot across differing tuples. Pick one canonical config for the trend
dashboard (suggest: `ubuntu-latest` + Debug, the `test` job) and treat the rest
as secondary series.

Note the CI matrix is `os: [ubuntu-latest, macos-latest]` across two jobs,
`test` and `jit`. The `jit` dimension is effectively "which job produced this
row" -- the `jit` job configures `-DTUR_JIT=ON` (`ci.yml:204`) -- though reading
it from the cache per Phase 2 still works and is more robust than inferring from
the job name.

---

## Phase 0 -- Emit machine-readable results

**Correction: there is no single "existing test step" to append to.** The `test`
job runs ctest twice, deliberately, because `tur_tests` is `RUN_SERIAL` and the
auxiliary suites are parallel:

```yaml
# ci.yml:107
run: ctest --output-on-failure --progress --test-dir build -R '^tur_tests$'
# ci.yml:129
run: ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --progress --test-dir build -E '^tur_tests$'
```

A single `--output-junit results.xml` would have the second invocation
**overwrite the first**, silently losing `tur_tests` -- the most valuable series
in the whole exercise. Write two files and merge in Phase 2:

```bash
ctest --output-on-failure --progress --test-dir build -R '^tur_tests$' \
      --output-junit results-main.xml
ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --progress \
      --test-dir build -E '^tur_tests$' --output-junit results-aux.xml
```

The `jit` job runs its own ctest step (`ci.yml:270`) and needs the same
treatment if its suites are to be tracked.

Requires CMake 3.21+ (runner images are fine; `cmake_minimum_required` in-tree
is 3.20 at `CMakeLists.txt:1` and stays there). Gives per-test name, status, and
duration with no changes to any harness script.

Acceptance: the two XML files together contain one `<testcase>` per registered
suite with a nonzero `time` attribute, and their union is the full 112.

---

## Phase 1 -- Make skips distinguishable from passes

The premise holds -- these suites exit 0 when they skip, CTest records a fast
PASS, and the duration series drops to near-zero, indistinguishable from a real
speedup. But the remedy needs rewriting, because **most skipping harnesses
already print a marker, in three mutually incompatible formats**, and the
worst-behaved ones print the word `PASS`:

| Convention | Harnesses |
| --- | --- |
| `SKIP <name>: reason` | `run-jit.sh`, `run-refine-fuzz-src.sh`, `run-refine-wasm.sh`, `run-tutorial-quickstart.sh`, `run-type-fuzz-src.sh` |
| `SKIP: reason` | `tests/lsp/run-mcp-lsp.sh` |
| **`PASS ...` (mislabels a skip as a pass)** | `run-build-project.sh`, `run-dap.sh`, `run-flags.sh`, `run-install.sh`, `run-turi.sh`, `run-phase4-gdb.sh`, `run-phase5-gdb.sh`, `run-scscm-compile.sh` |
| no marker at all | `run-bench.sh` |

Concretely, `run-phase4-gdb.sh:88` prints
`PASS phase4: gdb not available -- skipping native backtrace check`, and
`run-scscm-compile.sh:33` calls `pass "scscm-compile (turmeric-spices absent -- skipped)"`.
A grep-based ingest that trusted these would classify a skip as a pass forever.

So the change is a **normalization**, not a greenfield sentinel:

1. Settle on one machine-readable marker, distinct from the human `SKIP`/`PASS`
   prose already in use so the two never collide:

   ```bash
   echo "TUR_SKIP: emcc not found"
   exit 0
   ```

2. Convert all 13 harnesses above. The `PASS`-printing ones are the priority --
   they are actively wrong, independent of this plan. `run-bench.sh` needs a
   marker added from scratch.
3. Keep the existing human-readable line if desired; the ingest keys on the
   `^TUR_SKIP:` prefix only.

The ingest script greps captured stdout for `^TUR_SKIP:` and records
`status='skip'` plus the reason. Skipped rows are excluded from duration trends
but counted in a separate "suites actually run" series.

Note that JUnit output alone will not carry this -- ctest records these as
passes. The ingest needs the harness stdout, which means `--output-on-failure`
is not enough on a green run. Either add `-V`/`--verbose` (the `jit` job already
uses `-V` for exactly this "counts must reach the log on a passing run" reason,
`ci.yml:275-280`) or capture via `tee`.

Acceptance: with emcc absent, `tur_refine_wasm` (#57) lands as `skip`, not as a
0.1s pass; and `tur_phase4_gdb` (#20) does too rather than as a `PASS`.

---

## Phase 2 -- Ingest script

`tools/ci/collect-suite-timings.py` -- stdlib only, no deps. (`tools/ci/` does
not exist yet; create it.)

Input:
- `results-main.xml` **and** `results-aux.xml` (JUnit from ctest; accept N input
  files and merge, per Phase 0)
- captured ctest stdout, for the `TUR_SKIP` scan (Phase 1)
- env: `GITHUB_SHA`, `GITHUB_REF_NAME`, `GITHUB_RUN_ID`, `GITHUB_RUN_ATTEMPT`,
  `RUNNER_OS`
- build config: `build/CMakeCache.txt` for `CMAKE_BUILD_TYPE`, `TUR_JIT`,
  `TUR_DEBUG_SANITIZE` -- all three confirmed present as cache entries
- `nproc`

**Correction on the compiler dimension.** `CMakeCache.txt` holds only
`CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc` -- a path, and on this machine a
`cc` that is actually AppleClang 21. That is useless as a trend dimension: two
runs with different compilers can both say `/usr/bin/cc`. The id and version
live elsewhere, in `build/CMakeFiles/<cmake-ver>/CMakeCCompiler.cmake`:

```cmake
set(CMAKE_C_COMPILER_ID "AppleClang")
set(CMAKE_C_COMPILER_VERSION "21.0.0.21000101")
```

Read those two and compose `cc` as `"AppleClang-21.0.0"`. Glob the directory --
the CMake version in the path varies by runner image.

Output: newline-delimited JSON, one object per suite.

```json
{
  "sha": "...", "branch": "main", "run_id": "...", "run_attempt": 1,
  "ts": 1750000000,
  "build_type": "Debug", "os": "Linux", "cc": "AppleClang-21.0.0",
  "nproc": 4, "jit": false, "sanitize": true,
  "suite": "tur_tests", "status": "pass", "skip_reason": null,
  "duration_ms": 184000
}
```

Notes:
- JUnit `time` is seconds as a float; convert to int ms.
- CTest's `NOTRUN` / disabled entries map to `status='notrun'`.
- Suites absent from the merged XML entirely (conditionally not registered) are
  simply not emitted -- absence is meaningful and should not be backfilled as
  zero. With `TUR_JIT=OFF` this is the normal state for the four JIT suites.
- Guard against a suite appearing in **both** input files (a `-R`/`-E` pattern
  drift would do it). Prefer the first occurrence and warn; two rows for one
  suite in one run would corrupt any aggregate.

---

## Phase 3 -- Storage

### Option A (recommended start): orphan branch

- Orphan branch `ci-metrics`, single file `suite-timings.jsonl`.
- Workflow appends the new lines and force-pushes with a `concurrency` group to
  avoid clobbering.
- Free, permanent, diffable, greppable, no infrastructure.
- **~112 lines per run**, not 70 (and ~224/run once both `test` matrix legs
  report). At 20 runs/day that is ~1.6M lines/year across the matrix. Split by
  year (`suite-timings-2026.jsonl`) sooner than the original estimate implied,
  or restrict publishing to the canonical leg only (see Phase 4).

### Option B: Cloudflare Worker + D1

Use when the orphan file gets slow to query or a live dashboard is wanted.

```sql
CREATE TABLE suite_runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  sha TEXT NOT NULL,
  branch TEXT,
  run_id TEXT,
  run_attempt INTEGER,
  ts INTEGER NOT NULL,
  build_type TEXT,
  os TEXT,
  cc TEXT,
  nproc INTEGER,
  jit INTEGER,
  sanitize INTEGER,
  suite TEXT NOT NULL,
  status TEXT NOT NULL,        -- pass | fail | skip | notrun
  skip_reason TEXT,
  duration_ms INTEGER
);
CREATE INDEX idx_suite_ts ON suite_runs(suite, ts);
CREATE INDEX idx_run ON suite_runs(run_id);
```

Worker exposes `POST /ingest` and `GET /api/trends?suite=&days=`. Auth via
GitHub OIDC: request a token with `id-token: write`, validate the JWT against
the Actions JWKS in the Worker, check the `repository` claim equals
`rjungemann/turmeric`. Avoids a long-lived shared secret.

Volume is trivial (~112 rows/run), so no pruning is needed.

---

## Phase 4 -- Workflow wiring

In `.github/workflows/ci.yml`, on the canonical job only (`test`, and within it
the `ubuntu-latest` matrix leg). **`ci.yml` currently has no `permissions:`
block at all**, so one must be added rather than amended.

```yaml
permissions:
  contents: write      # orphan-branch option
  id-token: write      # OIDC option

concurrency:
  group: ci-metrics
  cancel-in-progress: false

steps:
  # (both existing ctest steps gain --output-junit, per Phase 0)

  - name: Collect timings
    if: always()
    run: |
      python3 tools/ci/collect-suite-timings.py \
        results-main.xml results-aux.xml > timings.jsonl

  - name: Publish timings
    if: always() && github.event_name == 'push' && github.ref == 'refs/heads/main' && matrix.os == 'ubuntu-latest'
    run: bash tools/ci/publish-timings.sh timings.jsonl
```

Rules:
- Record on failure too -- a suite that regressed to a timeout is exactly the
  signal wanted. Hence `if: always()` on collect.
- Publish only from `main` pushes, and only from the canonical matrix leg. PR
  runs (especially from forks, which get a read-only token) collect and print
  but do not write.
- The original plan put `continue-on-error: true` on the test step so collect
  would still run. **Prefer `if: always()` on the collect/publish steps
  instead.** `continue-on-error` on the test step makes the job green when tests
  fail, which is a real gate weakened for a metrics feature; the `jit` job
  already uses `continue-on-error` in a targeted, documented way
  (`ci.yml:271`) and that nuance should not be casually copied to the blocking
  `test` job. `if: always()` gets the same data without touching the gate.

---

## Phase 5 -- Dashboard

Static page reading the JSONL (or `/api/trends`). No build step; Chart.js or
uPlot from a CDN.

Views, in order of usefulness:

1. **Stacked area, top 10 suites by mean duration, last 90 days.** Immediately
   shows whether a regression is broad or one harness.
2. **Small-multiples sparkline grid, one per suite.** Scan for step changes.
   With 112 suites this needs a filter or a "top N + rest" fold.
3. **Suite table**: mean, p90, delta vs 30-day baseline, last status, sorted by
   absolute delta.
4. **Skip ledger**: which suites are currently skipping and why. A suite
   skipping for weeks is a coverage hole worth surfacing -- and given Phase 1's
   findings, several are likely skipping in CI right now without anyone knowing.

---

## Phase 6 (optional) -- Drill into `tur_tests`

`tur_tests` covers the whole fixture corpus and dominates total wall time
(CLAUDE.md documents ~1442 fixtures at ~4-5 min; the `jit` job's baseline notes
2414 fixtures on its own path). Suite-level tracking will show it grew but not
why.

Change `tests/run.sh` to write a per-fixture TSV alongside its normal output,
gated behind an env var so local runs are unaffected:

```
TUR_TIMING_OUT=fixture-timings.tsv bash tests/run.sh
```

Format: `fixture_name<TAB>duration_ms<TAB>status`.

Ingest into a second table `fixture_runs` with the same dimension columns. Same
treatment for `tests/run-turi.sh` if the interpreted corpus matters.

Caveats:
- Because run.sh fans out across `nproc`, per-fixture wall times include
  scheduling contention and are noisier than the suite total. Useful for ranking
  slowest fixtures, less so for small deltas.
- `run.sh` writes its summary to **stderr**, not stdout -- keep the TSV on its
  own file descriptor or path rather than muxing into either stream.
- Per CLAUDE.md, interpreted fixtures are memory-bound rather than CPU-bound
  (~4 KiB retained per trampolined step), so a `run-turi.sh` timing series will
  track peak RSS and co-scheduling more than it tracks real work. Record
  `nproc` and treat interpreted deltas with more suspicion than compiled ones.

---

## Non-goals

- Flake detection. Different problem, different data shape; revisit once timing
  trends are working.
- Coverage tracking.
- Tracking every OS/build-type matrix cell on the main dashboard. Collect them,
  chart one.

## Rollout order

1. **Phase 0** -- two flags (not one), immediate value, no risk.
2. **Phase 2 + 3A** -- script plus orphan branch; start accumulating now so
   there is history to look at later.
3. **Phase 1** -- skip normalization, before the data is trusted. Larger than
   originally scoped (13 harnesses, and 8 of them currently print `PASS` on a
   skip). The `PASS`-on-skip mislabeling is worth fixing on its own merits
   whether or not the rest of this plan proceeds.
4. **Phase 5** -- dashboard once ~2 weeks of data exists. (Shipped
   2026-08-28, three days in, as `/ci`.)
5. **Phase 6** -- only if `tur_tests` turns out to be the thing that moves.
   (Declined 2026-09-09: it did not; see the two-week readout.)

## Open question

Phase 1 will reveal which suites are skipping on the CI runners specifically.
If a suite has been skipping in CI since it was written, its timing series was
never the point -- the finding is the coverage hole, and it likely deserves its
own report under `docs/reported/` rather than a row on a dashboard.
