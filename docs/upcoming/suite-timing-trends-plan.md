# Plan: Test Suite Timing Trends for `rjungemann/turmeric`

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
4. **Phase 5** -- dashboard once ~2 weeks of data exists.
5. **Phase 6** -- only if `tur_tests` turns out to be the thing that moves.

## Open question

Phase 1 will reveal which suites are skipping on the CI runners specifically.
If a suite has been skipping in CI since it was written, its timing series was
never the point -- the finding is the coverage hole, and it likely deserves its
own report under `docs/reported/` rather than a row on a dashboard.
