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
separately ([`ci-cps-tramp-turi-timeouts-under-load.md`](../archive/ci-cps-tramp-turi-timeouts-under-load.md)
-- resolved 2026-08-01, the cause was ~3.5 GiB RSS per fixture under
`--interpret`, not CPU contention -- plus the real
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
locally on macOS -- which the report originally read as disagreeing with macOS
CI. **It does not disagree; see below.**

## RESOLVED for `Test (macos-latest)` (2026-08-01): it is the straddle report

The fixture names were extracted from run 2202 on `main`
(job 91324836416, head `8b1ea4380`). The AOT suite's tail is:

```
summary: 2495 passed, 4 failed
  - conv-defstruct-option-fn-element (build failed)
  - defalias-composite (build failed)
  - fn-value-matrix-ok-rows (build failed)
  - hkt-ap-fn-in-container (build failed)
```

That is **exactly** the four fixtures of
[`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md),
with exactly its two error shapes -- `incompatible pointer to integer
conversion passing 'void *' to parameter of type 'int64_t'` at
`ctor_Option__fn1_float__float(true, x)` (its open case A) and `incompatible
integer to pointer conversion returning 'int64_t'` at `return cons(...)` /
`return v;` / `return __env___env_1376->c;` (its open case B).

Three corrections follow, and they retire most of this report:

1. **There is no local-vs-CI disagreement.** The commit message's
   `AOT: 2495 passed, 4 failed` and CI's `summary: 2495 passed, 4 failed` are
   the same numbers. The "most interesting part of this report" was a
   misreading of its own evidence.
2. **The sanitizer/toolchain hypothesis is dead for this job.** The failures
   are `cc` rejecting emitted C at `-Wint-conversion`, which has nothing to do
   with ASan, Homebrew LLVM, or the startup deadlock. It reproduces at any
   sanitizer setting.
3. **The six-hour hang in run 2196 was not this.** `Test (macos-latest)`
   completed in 501s in run 2202. Whatever stalled 2196 was a one-off or a
   different phase; there is no standing hang to hunt.

So `Test (macos-latest)` is not an undiagnosed macOS-only defect. It is a
known, filed, platform-independent codegen bug that only Linux's warn-instead-
of-error posture hides. **Fixing the straddle report turns this job green.**

Corroborated on PR #753 (job 91329206395, head `5662aab4`): the same four
fixtures, the same two error shapes, and `summary: 2496 passed, 4 failed` --
the pass count moved by exactly +1 against main's 2495, which is that PR's one
added fixture. The failing set is stable and does not drift with unrelated
work, so it is a fixed, enumerable list rather than a flaky or load-dependent
one. That also makes this job usable as a regression signal despite being red:
a *fifth* name appearing, or a count that does not move by the number of
fixtures a PR adds, is real news.

## `JIT engine (macos-latest)` (2026-08-01): a harness bug, unrelated to the above

Identified, and it is **not** the straddles -- a completely different cause.
(The earlier note here said the log window could not be widened; that was
wrong. `tail_lines` does work, the `errors/*` skip list is just long enough
that ~560 lines are needed to clear it.)

From PR #753, job 91329206401:

```
jit fixture summary: 2008 passed, 407 failed, 47 skipped
  (of which 17 passed via the cc fallback -- TUR-W0070)
```

**All 407 failures are `errors/*` negative fixtures, every one reporting
`jit diagnostic mismatch`.** No positive fixture fails. That shape is the
whole diagnosis: negative fixtures are the only ones that go through
`run_jit_error_fixture`.

### Root cause -- `tests/run-jit.sh:308` called bare `timeout`

The harness already knows stock macOS ships no `timeout(1)` and probes for it
at `tests/run-jit.sh:109-118`, setting `_tur_timeout_bin` to `timeout`, else
`gtimeout`, else empty (run untimed). The positive-fixture path uses the
resulting `_run_timed` wrapper (`:235`, `:239`). **The negative-fixture path
was missed and still called bare `timeout 15`.**

On a runner without GNU coreutils that fails silently and totally:

```
$ ( PATH=/nonexistent timeout 15 sh -c 'echo DIAGNOSTIC >&2' ) 2>err || true
$ cat err
bash: timeout: command not found          <- not the diagnostic
```

`timeout` is not found, the command fails, the trailing `|| true` swallows it,
`jit.stderr` gets a shell error instead of a compiler diagnostic, and every
`expected.diag` needle misses. Every negative fixture then reports
`jit diagnostic mismatch` as though the compiler had stopped emitting
diagnostics at all.

This also explains the one place a local-vs-CI disagreement is *real*. The
macOS JIT leg was made blocking on a hand-measured local baseline of
`2414 passed, 0 failed, 47 skipped` (`.github/workflows/ci.yml:195-212`). A
developer Mac with Homebrew coreutils on `PATH` has `timeout` and is green; the
GitHub macOS runner does not and is not. The skip count matches exactly (47) and
the corpus totals agree, so nothing else differs -- and Linux is green because
`timeout` exists there.

Fixed in this PR: `:308` now uses `_run_timed`, matching the positive path. On
Linux this is a byte-for-byte no-op (`_run_timed 15 cmd` expands to
`timeout 15 cmd`), so it can only change the macOS result.

### CONFIRMED (2026-08-01): 407 -> 6

The first macOS JIT run carrying the fix (run 30685603734, job 91330616390,
head `03ec0d4a`):

```
jit fixture summary: 2409 passed, 6 failed, 47 skipped
```

Every `errors/*` negative fixture now passes. 401 of the 407 were the harness
bug; the diagnosis holds.

The **6 residuals are real JIT findings this bug was masking** -- all GC / `Rc`
/ weak-reference fixtures, all macOS-only (the Linux JIT leg is genuinely
green, ctest target passing outright rather than `continue-on-error` hiding
it). They were failing on `main` too, buried in the 401. Filed separately as
[`jit-macos-gc-rc-weak-fixtures-fail`](jit-macos-gc-rc-weak-fixtures-fail.md);
they are not this report's problem and it does not wait on them.

## Fix directions

1. **Upload the harness stdout (or `tests/**/actual.*`) as a CI artifact on
   failure.** Still worth doing -- both halves of this report cost a log-window
   fight to diagnose.
2. Fix [`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
   and re-check: that should take `Test (macos-latest)` green.
3. ~~JIT half~~ -- **done.** Fixed in PR #753 (`tests/run-jit.sh:308`),
   confirmed by run 30685603734 (407 -> 6). The 6 residuals moved to
   [`jit-macos-gc-rc-weak-fixtures-fail`](jit-macos-gc-rc-weak-fixtures-fail.md).

**This report closes when the AOT half (the straddles) is fixed.** Neither half
was a macOS-platform defect: one is a filed codegen bug that Linux only warns
about, the other was a test-harness portability bug. Its original framing -- an
undiagnosed macOS-only mystery needing someone with a Mac -- was wrong on both
counts, and no macOS box was needed to resolve either. (The GC/Rc report it
spawned *does* need one, but that is a different, genuinely platform-specific
finding.)

## Fix directions

1. **Upload the harness stdout (or `tests/**/actual.*`) as a CI artifact on
   failure.** This is now the only thing standing between here and naming the
   JIT failures -- the log-tail window cannot be widened from the API side.
2. Fix [`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
   and re-check: that should take `Test (macos-latest)` green and leave only
   the JIT job.
3. If the JIT job turns out to be the straddles too, this report closes
   entirely as a duplicate of that one.

~~Reconcile the local-macOS vs macOS-CI disagreement~~ -- struck: there is no
disagreement (correction 1 above).
