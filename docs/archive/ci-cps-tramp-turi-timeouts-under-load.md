# `cps-tramp-resume-*` fail the turi suite under load, pass in isolation

> **RESOLVED 2026-08-01 -- Fix direction 3, and the mechanism was not CPU
> contention.** Measured on the report's own 4-core/16 GiB configuration, each
> of the two fixtures peaks at **~3.5 GiB RSS** under `--interpret` (the
> compiled path is ~70 MiB), and they are *deterministically* co-scheduled by
> `tests/run-turi.sh`. Two of them plus two other workers is memory pressure on
> a 16 GiB runner, and the interpreter's wall clock is superlinear in RSS. Both
> fixtures now run at 1/10th depth on every path, with the full depth preserved
> in compiled-only siblings. Separately, both harnesses were reporting a
> run-phase timeout as a **"stdout mismatch"**, which is what made this look
> like a wrong answer. Details in *Resolution* at the end.

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

## Did not reproduce 2026-08-01 -- still open, but the bar has moved

Two consecutive full-parallelism `bash tests/run-turi.sh` runs on a 4-core
Linux box (the report's own repro configuration) both came back:

```
turi fixture summary: 1699 passed, 1 failed, 697 skipped
FAIL hkt-constrained-byvalue-bind-pure -- stdout mismatch
```

Both `cps-tramp-resume-deep` and `cps-tramp-resume-multicase` passed. The only
red is the real interpreter defect
([`turi-return-directed-method-keeps-baked-instance`](turi-return-directed-method-keeps-baked-instance.md)),
which is what the "one real filed failure" line below refers to.

Nothing was fixed: `expected.timeout` is still 60 on both fixtures, there is
still no `requires.serial` marker, and the trampoline step count is unchanged.
So this stays open -- an intermittent timing failure is not disproved by two
green runs, and `Test (ubuntu-latest)` is still red on `main` at the
`turi_fixture_tests` target. But the margin on this hardware is evidently not
thin, which points at the CI runner being slower than a 4-core container rather
than at 60s being universally too tight. Whoever picks this up should get the
per-fixture timing off a failing CI run before choosing between the fix
directions below -- fix 1 is a guess without it.

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

## Resolution

### The premise was wrong: it is memory, not CPU

The report asks for per-fixture timing off a failing CI run before choosing a
fix, on the theory that "every worker is contending" for CPU. That timing does
not exist -- neither harness records or prints elapsed time -- so the mechanism
was measured directly instead, on the report's own configuration (4 cores,
16 GiB, Debug + ASan):

| | wall clock | peak RSS |
|---|---|---|
| `cps-tramp-resume-deep`, `--interpret`, idle box | 28.1 s | **3535 MiB** |
| `cps-tramp-resume-multicase`, `--interpret`, idle box | 12.2 s | **3598 MiB** |
| `cps-tramp-resume-deep`, compiled (`tur run`) | 2.5 s | 71 MiB |

CPU contention is not the problem. Running four copies of the deep fixture
concurrently on four cores -- exactly what `xargs -P "$JOBS"` does -- took
**25.6 s**, i.e. no slower than idle: one process per core, each with its own
core. The suite does not oversubscribe CPU.

Memory is the problem, and it is severe. At **eight** concurrent copies the
same measurement took **266 s** -- 4.4x past the 60 s timeout -- and the kernel
OOM-killed four of the eight. The interpreter's wall clock is superlinear in
its own RSS:

| N (steps) | `--interpret` time | peak RSS |
|---|---|---|
| 100,000 | 0.9 s | 477 MiB |
| 200,000 | 1.6 s | 842 MiB |
| 500,000 | 3.7 s | 2050 MiB |
| 1,000,000 | 28.1 s | 3535 MiB |

Doubling 5e5 -> 1e6 costs 7.6x the time, not 2x. Past roughly 2 GiB the
allocator and the page cache start losing, and the curve leaves the timeout
behind.

The cause of the RSS is not a leak. The tree-walking interpreter's closures and
continuations are process-lifetime by design (CLAUDE.md, leak-detection policy),
so a trampoline retains roughly **4 KiB per resumed step**. The step count is
therefore a memory multiplier under `--interpret` and nothing at all on the
compiled path, where the same program is O(1) memory.

### Why it is intermittent, and why it reproduces on demand

`tests/run-turi.sh` feeds a sorted directory list to `xargs -P "$JOBS"`. The two
fixtures sit about ten entries apart, and every entry between them finishes in
under a second while `deep` runs for 28 s -- so `multicase` reliably starts
while `deep` is still going. They are co-scheduled by construction, ~7 GiB
between them, on a runner that also holds a page cache and a ccache. Whether
that tips into thrashing depends on what else is resident, which is exactly the
"passes on a re-run" behaviour the 2026-08-01 note recorded. The margin was
never wide; it was 2.1x on an idle box and negative under pressure.

### Fix: direction 3, applied per path

Direction 1 (raise the timeout) treats the clock and leaves 7 GiB of concurrent
RSS in place. Direction 2 (`requires.serial`) would work but costs ~40 s of
serial wall clock and, on the CPU theory it was proposed under, would have
bought nothing -- the measurement above shows CPU serialization is not what
these fixtures need. Direction 3 addresses the multiplier itself.

"Under the interpreter specifically" is done by splitting each fixture in two
rather than by teaching the harness a new marker:

| Fixture | Steps | Paths | `--interpret` cost |
|---|---|---|---|
| `cps-tramp-resume-deep` | 1e5 | all | 0.9 s / 477 MiB |
| `cps-tramp-resume-deep-1m` | 1e6 | compiled only | -- |
| `cps-tramp-resume-multicase` | 5e4 | all | 0.9 s / 459 MiB |
| `cps-tramp-resume-multicase-500k` | 5e5 | compiled only | -- |

Peak concurrent RSS from this pair drops from ~7 GiB to ~0.9 GiB. Nothing is
lost: the C-stack flatness the deep fixtures prove is a property of the
compiled DK path, and the interpreter does not use the C stack for recursion
depth at all -- a non-tail `(+ 1 (down (- n 1)))` recursion interprets fine at
640,000 levels on an 8 MiB stack. Under `--interpret` these fixtures were only
ever re-checking the resumed-value arithmetic and the two-case dispatch, both of
which the small-N halves still check.

`requires.compiled` was used for the compiled-only halves rather than a new
marker. Note it is not purely a turi-side skip -- in `tests/run.sh` it also
switches the fixture from `tur run` to a separate `tur build` + exec. That is
why the split adds new fixtures instead of marking the existing ones: marking
them would have silently changed how the compiled path runs them too.

### A timeout was being reported as a wrong answer

Both harnesses checked `expected.stdout` before looking at the exit code, so a
`timeout(1)` kill (rc 124) left partial stdout that fell through to the diff and
was reported as `stdout mismatch`. `tests/run.sh` already special-cased rc 124
for its `emit-c` and `build` phases but not for the run phase; `run-turi.sh` did
not special-case it anywhere.

That is very likely why this report opens by ruling out a wrong answer: the
failure text said "stdout mismatch", which is a claim about output, not time.
Both harnesses now check rc 124 ahead of the stdout diff and print
`timed out (>Ns)`. Verified against a deliberately non-terminating fixture on
all three execution paths (`tur run`, `requires.compiled` exec, `--interpret`).
No fixture expects exit 124, so the check is unambiguous.

### Verification

- `bash tests/run.sh` -- 2503 passed, 0 failed.
- `TUR_FORCE=1 bash tests/run-turi.sh` -- 1701 passed, 0 failed, 697 skipped
  (cache bypassed, full parallelism).

Incidentally, `hkt-constrained-byvalue-bind-pure` -- the "one real filed
failure" this report cites as its reason to care, tracked as
[turi-return-directed-method-keeps-baked-instance](../archive/turi-return-directed-method-keeps-baked-instance.md)
-- also passed in that run. Nothing here touched it; it is noted only so the
next reader does not take this green run as evidence about that report.

`Test (ubuntu-latest)` on `main` is likewise no longer red: run 30689950481
(54fef281, 2026-08-01) completed both "Run fixture suite" and "Run auxiliary
suites" successfully. The "still red on `main`" line above is stale.
