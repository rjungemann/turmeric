# `cps-tramp-resume-*` fail the turi suite under load, pass in isolation

**Severity: low.** Test-harness flakiness, not a product defect. Two fixtures
produce byte-correct output but exceed their per-fixture timeout when the
interpreter suite runs at full parallelism, so `tests/run-turi.sh` (and the
`turi_fixture_tests` ctest target that wraps it) fails intermittently.

Found 2026-07-31 while triaging CI on PR #752.

## Affected

```
cps-tramp-resume-deep
cps-tramp-resume-multicase
```

## Repro

Full suite on a 4-core box:

```sh
bash tests/run-turi.sh
# turi fixture summary: 1696 passed, 3 failed, 697 skipped
# failed:
#   - cps-tramp-resume-deep
#   - cps-tramp-resume-multicase
#   - hkt-constrained-byvalue-bind-pure     <- unrelated, filed separately
```

The same two fixtures, run alone:

```sh
TUR_TEST_FILTER=cps-tramp bash tests/run-turi.sh
# turi fixture summary: 29 passed, 0 failed, 1 skipped
```

## Why it is a timeout, not a wrong answer

The recorded output matches expected exactly -- there is no divergence to
diagnose:

```sh
$ head -1 tests/fixtures/cps-tramp-resume-deep/expected.stdout
3000000
$ head -1 tests/fixtures/cps-tramp-resume-deep/turi.stdout
3000000
$ cat tests/fixtures/cps-tramp-resume-deep/expected.timeout
60
$ nproc
4
```

`turi.stderr` is empty. The fixture drives a 3,000,000-step trampoline
through the tree-walking interpreter, which is the slowest execution path in
the tree; 60s is enough when it has a core to itself and not enough when
`tests/run-turi.sh` has fanned out across `nproc` and every worker is
contending.

This is the failure mode CLAUDE.md already warns about under "Overlapping
runs cause false FAILURES, not just slowness" -- the novelty here is that the
contention is *internal* to a single suite run rather than caused by a second
concurrent run, so there is no second process to notice.

## Why it matters

It is a recurring false red. `Test (ubuntu-latest)` on PR #752 fails on
`turi_fixture_tests`, and these two are part of why. A CI check that fails for
timing reasons trains people to ignore it, which is expensive on a suite that
also carries one real filed failure
(`turi-hkt-constrained-byvalue-bind-pure-wrong-values.md`).

## Fix directions

Any of these would do; the first is the smallest:

1. Raise `expected.timeout` on the two fixtures. 60s is already
   fixture-specific, so this is just a bigger number for the slowest path --
   but it is a guess against unknown CI hardware, so prefer 2 if the cost is
   acceptable.
2. Mark them to run serially (the harness would need a `requires.serial`-style
   marker; `tests/run.sh` and `tests/run-turi.sh` are both already
   `RUN_SERIAL` at the ctest level, so this would be a within-suite notion).
3. Shrink the trampoline step count under the interpreter specifically. The
   fixture is testing that deep resumption does not blow the stack; 3,000,000
   is far past the point where that is demonstrated, and a smaller count would
   still fail loudly if the trampoline regressed.

Do not simply delete the timeout: an untimed fixture turns a genuine
interpreter infinite loop into a hung suite, which is the thing the per-fixture
timeout exists to prevent.
