# Test Runner Contract (Deferred Follow-up)

This document defines the expected behavior for stdlib test running follow-up work.

## Scope

Applies to:
- stdlib test helpers in `stdlib/test.tur`
- CLI test entry behavior (future `tur test` command path)
- fixture and output conventions for pass/fail reporting

## Discovery Model

1. Directory mode scans `tests/fixtures/**` for test-bearing fixture directories.
2. A fixture is considered runnable if it has `input.tur`.
3. Error fixtures are identified by `tests/fixtures/errors/**` and validated against `expected.diag`.
4. Non-error fixtures are validated against `expected.stdout` where provided.

## Registration Model

1. `deftest` registers test definitions in a global registry in declaration order.
2. `run-tests!` executes registered tests in deterministic order (declaration order).
3. Duplicate test names are rejected with a diagnostic.

## Assertion Semantics

- `assert expected actual`: pass iff values are equal.
- `assert-true x`: pass iff `x` is boolean true.
- `assert-false x`: pass iff `x` is boolean false.
- `assert-nil x`: pass iff `x` is nil.
- `assert-error body`: pass iff evaluating `body` emits an error per test harness policy.

## Callback Callability Contract (v1)

- Test callbacks are passed through function parameters typed as `:ptr<void>`.
- Passing a function value where `:ptr<void>` is expected is supported.
- Passing `nil` where `:ptr<void>` is expected is supported as a null-callback convention.
- In v1, callback invocation through this bridge is arity-0 only.
- Non-zero-arity invocation through `:ptr<void>` is a compile-time arity mismatch.
- Callback calls lower to typed function-pointer casts in emitted C.

## Process Exit Semantics

- Exit code `0`: all tests pass.
- Exit code `1`: at least one test fails.
- Exit code `2`: harness/runtime/configuration error prevented reliable execution.

## Output Contract

Streaming output:
- `.` per passing test
- `F` per failing test

Summary output at end:
- total tests
- passed
- failed
- elapsed (optional)

Failure detail block includes:
- test name
- assertion message / diagnostic snippet
- source location if available

## Stdout/Stderr Split

- Progress markers and summary go to stdout.
- Diagnostics, harness errors, and failure details may go to stderr.
- Contract tests should match whichever stream is explicitly specified for a fixture.

## Determinism Requirements

- Test order is stable.
- Output order is stable.
- Summary counts are reproducible.

## Follow-up Work Hooks

- Integrate this contract with `stdlib/test.tur` implementation work.
- Integrate with future `tur test` CLI command behavior and fixture validations.
