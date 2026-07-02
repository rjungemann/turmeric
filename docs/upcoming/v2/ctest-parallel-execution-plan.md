# Parallelize CTest Execution

* **Status**: Proposed
* **Extracted from**: `docs/archive/test-performance-optimization-plan.md` (Phase 1)
* **Date**: 2026-07-01

## 1. Problem Statement

The root `Justfile` runs CTest sequentially. With 68 registered test targets
(including massive compiled and interpreted fixture runners), the cumulative
runtime is bound to the sequential sum of all targets rather than the single
slowest component.

Sequential execution meaningfully inflates end-to-end suite runtime (~7m10s
baseline) even though the underlying targets are independent and could run
concurrently.

## 2. Objectives & Goals

- Bound overall test execution duration by the single slowest test runner
  (`tests/run.sh` cold-recompilation, ~5.5 minutes) rather than the sequential
  sum of all targets.
- Preserve correctness and stability across the suite -- no test target may
  depend on being run in isolation from its siblings.

## 3. Technical Design

Currently, the `Justfile` invokes CTest sequentially:

```justfile
test: build doctest
    timeout 300 ctest --output-on-failure --progress --test-dir build
```

Modern CTest supports execution parallelism using all active logical cores
when invoked with the `--parallel` or `-j` options without a specific thread
limit.

Modify the `test` recipe in `Justfile` to run CTest in parallel:

```justfile
test: build doctest
    timeout 300 ctest -j --output-on-failure --progress --test-dir build
```

*Technical Rationale*: Under parallel CTest, the overall test execution
duration is bounded by the single slowest test runner rather than the
sequential sum.

## 4. Verification and Validation

1. Confirm end-to-end success of all 68 registered ctest targets under `-j`.
2. Confirm no target regresses due to cross-target contention (temp dirs,
   shared cache paths, port collisions, etc.). Any target that requires
   isolation must declare a `RESOURCE_LOCK` in its `add_test` registration.
3. Measure wall-clock delta vs. sequential baseline; expect end-to-end to
   collapse toward the wall-clock of `tests/run.sh` alone.

## 5. Rollout Strategy

1. Update the `test` recipe in `Justfile` with `-j`.
2. Run `tur run test` and inspect for any target that fails under parallel
   execution but passes serially -- fix by adding a resource lock rather
   than reverting the parallelism.
3. Measure and record the new baseline runtime.
