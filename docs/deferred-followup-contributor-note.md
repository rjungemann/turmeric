# Deferred Follow-up Contributor Note

This note explains how to work on deferred backlog items and run targeted checks quickly.

## Backlog Source

Primary backlog tracker:
- [docs/deferred-tasks-phase7-phase11.md](docs/deferred-tasks-phase7-phase11.md)

## Fixture Naming Convention

Use existing fixture style:

- Happy path fixture:
  - `tests/fixtures/<name>/input.tur`
  - `tests/fixtures/<name>/expected.stdout`

- Error fixture:
  - `tests/fixtures/errors/<name>/input.tur`
  - `tests/fixtures/errors/<name>/expected.diag`

- Optional generated artifacts during runs:
  - `actual.stdout`
  - `actual.stderr`
  - `actual.c`

## Expected Output Format Rules

1. `expected.stdout` is exact text output comparison.
2. `expected.diag` uses substring matching (one expected substring per line).
3. Keep diagnostics stable and actionable; include key error phrase and symbol/type names.

## Snapshot Strategy Decision

Decision: keep snapshots colocated with fixture folders (no separate global snapshot tree).

Rationale:
- follows existing fixture organization
- keeps ownership and review context local to each feature
- minimizes path churn in test tooling

## Fast Local Workflow

From repo root:

1. Build:
- `make`

2. Run full fixture harness:
- `bash tests/run.sh`

3. Run targeted fixture checks via grep (example):
- `bash tests/run.sh 2>&1 | grep "ref-move\|copy-traits\|borrow"`

4. Compile one file quickly:
- `./build/tur emit-c path/to/input.tur`

5. Execute one file quickly:
- `./build/tur run path/to/input.tur`

## Working Agreement

When completing a backlog checkbox:
1. Implement behavior.
2. Add/adjust fixtures.
3. Verify local run.
4. Mark checkbox in backlog file only after validation.
