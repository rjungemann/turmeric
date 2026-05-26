# Plan: Strict per-case and whole-suite test timeouts

> **Status:** Draft Plan
> **Last Updated:** 2026-05-26
> **Type:** Test infrastructure / CI / Developer UX

---

## Overview

The repository already has **partial** timeout coverage, but it is not yet
strict or uniform:

- `just test` wraps the whole `ctest` invocation in a 5-minute shell timeout
  (`Justfile`), but that protection only exists on that one entrypoint.
- `tests/run.sh` has per-fixture runtime timeouts, but:
  - they default to 10 seconds rather than the desired 30,
  - negative fixtures are not timed,
  - codegen/build steps are not budgeted as part of one strict per-case wall
    clock,
  - `expected.timeout=0` currently disables the timeout entirely.
- `tests/run-turi.sh` has a 15-second runtime timeout for fixture execution, but
  it does not use the same cross-platform timeout helper as `run.sh`, and its
  async follow-up scripts are not timed individually.
- `tools/run-doctests.sh`, `tests/run-cli.sh`, `tests/run-flags.sh`,
  `tests/check-span-unknown.sh`, `tests/turi/eval-test.sh`, and the other
  shell-based suites do not consistently enforce a per-case 30-second cap.
- `CMakeLists.txt` registers many `add_test(...)` entries, but almost none have
  explicit CTest timeout properties.

The desired end state is:

1. **No logical test case may run longer than 30 seconds.**
2. **No full suite invocation may run longer than 5 minutes.**
3. These limits apply consistently whether the suite is invoked via `just test`,
   direct shell scripts, or `ctest`.

---

## Requirements

### Hard requirements

1. Every logical test case has a strict **30-second wall-clock timeout**.
2. The full suite has a strict **300-second wall-clock timeout**.
3. Timeouts work on both Linux and macOS.
4. Timeout failures are reported as ordinary test failures with clear case names.
5. Timeout enforcement must kill child processes as well as the immediate shell
   command when possible.

### Non-goals

- Reworking the test suite structure beyond what is needed to enforce timeouts.
- Speeding up the suite for its own sake, except where needed so the 5-minute
  cap becomes realistic.
- Replacing existing harnesses with a new external test runner.

---

## Current timeout surfaces

### 1. Top-level suite entrypoints

- `Justfile`
  - `just test` currently runs `timeout 300 ctest --output-on-failure --progress --test-dir build`.
  - This already encodes the target suite cap, but only for that one command.
- `CMakeLists.txt`
  - registers `tur_tests`, `tur_cli_tests`, `tur_span_tests`,
    `tur_spice_resolver_tests`, `tur_flags_tests`, `turi_fixture_tests`,
    REPL/eval tests, and several async eval scripts.
  - No broad `TIMEOUT` policy is currently attached to these tests.

### 2. Multi-case shell runners

- `tests/run.sh`
  - logical cases are fixtures under `tests/fixtures/**`
  - currently only the **runtime** portion of happy-path fixtures is timed
  - negative fixtures are not timed
  - `expected.timeout=0` permits unlimited runtime
- `tests/run-turi.sh`
  - logical cases are fixture runs plus `tests/turi/eval-async-*.sh`
  - fixture runtime is timed, but not via the shared helper used by `run.sh`
  - async follow-up scripts are not timed per script
- `tools/run-doctests.sh`
  - logical cases are generated doctest examples, grouped by module
  - no timeout budget is enforced today
- `tests/run-cli.sh`
  - logical cases are directories under `tests/cli/*`
  - no per-case timeout today
- `tests/run-flags.sh`
  - logical cases are individual bash blocks in one file
  - no per-case timeout today
- `tests/check-span-unknown.sh`
  - currently one logical case, untimed
- `tests/turi/eval-test.sh`, `tests/turi/repl-smoke.sh`,
  `tests/turi/eval-effects.sh`, `tests/turi/eval-s4.sh`,
  `tests/turi/eval-tco.sh`, `tests/turi/eval-async-*.sh`
  - many contain multiple logical checks with no shared timeout harness

---

## Design principles

### Define "test case" at the harness level

The repo mixes:

- **one-CTest-test == one logical case** (for some small binaries/scripts), and
- **one-CTest-test == many logical cases** (for fixture runners and flag suites).

The plan should treat the **logical case** as the unit that gets the 30-second
budget, even when several cases live inside one shell script.

### Keep the 5-minute suite cap layered

The suite cap should be enforced at more than one layer:

1. outer suite invocation (`just test`, CI, or wrapper script) — 300 seconds
2. CTest invocation where applicable — 300 seconds
3. individual logical cases inside harness scripts — 30 seconds

This avoids relying on a single wrapper and keeps direct script execution honest.

### Prefer one shared timeout library

`tests/run.sh` already has a cross-platform timeout probe and helper. The rest
of the suite should not keep growing ad hoc timeout logic. A small shared shell
library should provide:

- timeout binary detection (`timeout`, `gtimeout`, fallback)
- case timeout execution
- whole-suite timeout execution
- consistent timeout exit-code normalization and reporting

---

## Recommended approach

### Phase TT1 -- Introduce a shared timeout helper for shell test harnesses

**Goal.** Every shell-based test harness can call the same timeout API.

**Changes.**

- Add a shared helper such as `tests/lib/timeout.sh`.
- Move the timeout detection logic out of `tests/run.sh` into that helper.
- Provide small primitives along these lines:
  - `tur_case_timeout_secs` (default 30)
  - `tur_suite_timeout_secs` (default 300)
  - `run_with_timeout <secs> <command...>`
  - `run_case_with_timeout <case-name> <command...>`
- Standardize timeout failure handling so the harness can say
  `FAIL <name> -- timed out after 30s`.

**Why first?**

This keeps the rest of the rollout mechanical. Without a shared helper, each
script will solve the same portability problem differently.

**Acceptance checks.**

1. The helper works on Linux with `timeout`.
2. The helper works on macOS with `gtimeout` if present.
3. The fallback path remains available when neither command exists.

### Phase TT2 -- Make `tests/run.sh` enforce a strict 30-second case budget

**Goal.** Each fixture in the compiled-fixture runner has a strict 30-second
wall-clock cap for the entire logical case, not just for the child binary.

**Changes.**

- Treat each fixture (`run_happy`, `run_negative`) as the timed unit.
- Wrap the full case body in a 30-second budget, including:
  - `emit-c`
  - `build`
  - compiled execution
  - interpreter execution
  - negative-case elaboration/codegen failure checks
- Replace the current `expected.timeout=0` semantics with one of:
  - **preferred:** remove unlimited mode entirely, or
  - allow only values `1..30`, rejecting anything larger or equal to zero.
- Keep `expected.timeout` only as a **narrower-than-default** override, never a
  wider one.
- Preserve parallel fixture execution, but ensure timeout cleanup does not leave
  orphan child processes behind.

**Acceptance checks.**

1. A hung happy-path fixture fails after 30 seconds.
2. A hung negative fixture fails after 30 seconds.
3. A fixture with `expected.timeout=5` still fails after 5 seconds.
4. `expected.timeout=45` or `0` is rejected (or clamped with a clear failure).

### Phase TT3 -- Bring `tests/run-turi.sh` to the same strict model

**Goal.** Every interpreted fixture and every async follow-up script has a
strict 30-second cap.

**Changes.**

- Reuse the shared timeout helper from TT1.
- Raise the default per-fixture timeout from 15 to the new strict default of 30.
- Apply the cap to the entire logical case, not just the `tur run` call.
- Time each `tests/turi/eval-async-*.sh` script individually.
- Decide whether `expected.timeout` in turi fixtures follows the same
  "may narrow but never widen" rule as `run.sh`.

**Acceptance checks.**

1. A hung turi fixture fails after 30 seconds.
2. A hung `eval-async-*.sh` script fails after 30 seconds.
3. Timeout reporting uses the same wording/style as `run.sh`.

### Phase TT4 -- Add strict timeouts to the remaining shell-based suites

**Goal.** Every logical case in the non-fixture shell suites is covered.

**Target scripts.**

- `tests/run-cli.sh`
- `tests/run-flags.sh`
- `tests/check-span-unknown.sh`
- `tools/run-doctests.sh`
- `tests/turi/eval-test.sh`
- `tests/turi/repl-smoke.sh`
- `tests/turi/eval-effects.sh`
- `tests/turi/eval-s4.sh`
- `tests/turi/eval-tco.sh`
- `tests/spice-resolver-tests.sh`

**Recommended pattern.**

- For scripts that already have a per-case helper (`run_cli_case`, `check`,
  `pass`/`fail` blocks), wrap that helper in `run_case_with_timeout`.
- For large one-file suites like `run-flags.sh`, refactor repeated case bodies
  into a helper such as:

  ```sh
  run_flag_case "tur-explain-kind-mismatch" bash -c '...'
  ```

- For doctests, choose one of two models:
  1. **module budget** — 30 seconds per generated doctest module file
  2. **example budget** — 30 seconds per individual doctest example

  Recommendation: start with **module budget** to keep the rollout tractable,
  but structure the helper so per-example timeouts remain possible later.

**Acceptance checks.**

1. Every shell script invoked by CTest uses the shared timeout helper.
2. Every logical case name appears in timeout failure output.
3. Doctest hangs fail fast instead of wedging the doctest run.

### Phase TT5 -- Add explicit CTest timeout policy

**Goal.** CTest itself knows the suite timeout expectations instead of relying
only on outer shell wrappers.

**Changes.**

- In `CMakeLists.txt`, add `TIMEOUT` properties for tests that are already
  one-logical-case-per-CTest-entry.
- For aggregated scripts:
  - give the outer CTest entry a budget aligned with its role in the suite
    (not necessarily 30 seconds, since the inner harness is enforcing 30 per
    logical case), and
  - document that the real per-case guarantee lives inside the script.
- Consider setting a CTest-wide default for single-case tests while overriding
  the aggregated suites explicitly.

**Important nuance.**

The outer CTest tests `tur_tests`, `turi_fixture_tests`, and `tur_flags_tests`
cannot simply be set to `TIMEOUT 30` unless they are split into smaller CTest
entries. They currently aggregate many logical cases. The plan should therefore
distinguish:

- **logical case timeout** = 30 seconds
- **aggregator CTest entry timeout** = larger bounded value
- **whole suite timeout** = 300 seconds

**Acceptance checks.**

1. Direct `ctest --test-dir build` still enforces bounded execution.
2. Single-case CTest tests time out at 30 seconds.
3. Aggregated CTest tests have explicit non-infinite bounds.

### Phase TT6 -- Standardize the 5-minute whole-suite cap across all entrypoints

**Goal.** The 300-second suite cap applies consistently in `just test`, CI, and
direct local invocations.

**Changes.**

- Keep `just test` at 300 seconds, but make the wrapper cross-platform rather
  than assuming GNU `timeout` exists.
- Add a dedicated suite wrapper script if needed, for example
  `tests/run-all-with-timeout.sh`, that:
  - enforces the 300-second global limit
  - invokes `ctest --output-on-failure --progress --test-dir build`
  - reports "suite timed out after 300s" clearly
- Update CI entrypoints to use the same wrapper instead of re-encoding timeout
  policy ad hoc.

**Acceptance checks.**

1. `just test` fails after 300 seconds on a wedged suite.
2. The same 300-second cap applies when running the canonical CI test command.
3. The wrapper cleans up child processes on timeout.

---

## Key design decisions to settle during implementation

### 1. Should `expected.timeout` survive?

Recommendation:

- keep the file, but only as a **tighter override**
- reject `0` (unlimited) and any value `> 30`

That preserves useful short-case tuning without undermining the strict policy.

### 2. What is the doctest "case" unit?

Recommendation:

- short term: each generated doctest module file is one case
- later, if needed, split to per-example budgets

Per-example budgeting is more precise, but module-level budgeting is much
cheaper to land.

### 3. Should aggregated CTest suites be split?

Recommendation:

- **not in the first pass**
- enforce 30 seconds at the inner logical-case level first
- only split into many `add_test(...)` entries later if CTest reporting or
  CI flake triage still needs finer granularity

This keeps the timeout rollout bounded.

---

## Validation matrix

1. **Compiled fixture runner**
   - happy fixture timeout
   - negative fixture timeout
   - per-fixture tightened override
2. **Turi fixture runner**
   - fixture timeout
   - async script timeout
3. **Shell suites**
   - CLI case timeout
   - flag-case timeout
   - REPL/eval case timeout
   - doctest module timeout
4. **CTest**
   - direct `ctest` respects explicit bounds
5. **Whole suite**
   - canonical suite command times out at 300 seconds

---

## Exit criteria

This plan is complete when:

- every logical test case in repo-owned harnesses has a strict timeout of at
  most 30 seconds,
- no supported test entrypoint allows an unlimited case timeout,
- the entire suite is bounded at 300 seconds across `just test`, CTest-based
  local runs, and CI,
- timeout failures identify the specific logical case that hung,
- the timeout implementation is centralized rather than duplicated across
  scripts.
