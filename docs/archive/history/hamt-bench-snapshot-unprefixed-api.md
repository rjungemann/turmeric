# `hamt-bench-snapshot.tur` called a `hamt_*` API that no longer exists

**Severity:** low (a benchmark, not shipped code) -- but it had been silently
un-runnable for some time, and it is a baseline the solver-extension plan's
SX0(a) curve depends on.

**Status:** RESOLVED in the SX0(a) change.

## Symptom

```
$ tur build benchmarks/hamt-bench-snapshot.tur -o /tmp/hbs
warning: implicit declaration of function 'hamt_new'; did you mean 'hamt_slnew'?
warning: implicit declaration of function 'hamt_set'; did you mean 'hamt_slget'?
warning: implicit declaration of function 'hamt_free'; did you mean 'hamt_slfree'?
warning: implicit declaration of function 'hamt_count'; did you mean 'hamt_slcount'?
collect2: error: ld returned 1 exit status
```

The benchmark compiled its inline C with implicit declarations (a warning, not
an error, under the flags in use) and then failed at LINK time, so the failure
surfaced late and looked like a toolchain problem rather than a stale API call.

## Root cause

The runtime's persistent-map entry points are `tur_hamt_new`, `tur_hamt_set`,
`tur_hamt_free`, `tur_hamt_count` (`src/runtime/hamt.h:119-136,280`), and the
emitted preamble declares exactly those. `benchmarks/hamt-bench-snapshot.tur`
was still calling the unprefixed `hamt_*` spellings, which no longer exist
anywhere. Nothing referenced the file from a test target, so the rename that
introduced the prefix left it behind without anything going red.

## Fix

Renamed the four call sites to the `tur_hamt_*` spellings. No local `extern`
declarations were needed -- the preamble already declares them, and adding a
second set produced conflicting-type errors (the preamble uses
`void *`/`int64_t`, not `Hamt *`/`uint64_t`).

The benchmark now builds and prints `101`, which is the "100 base entries + 1
new" the file's own comment says to expect.

## Worth noting

`benchmarks/` is not covered by `tests/run.sh`, so a benchmark can rot without
any signal. The two SX0 sweep scripts (`run-cap-sweep.sh`,
`run-capture-curve.sh`) both build what they measure and fail loudly if the
build fails, which covers the files they touch; the rest of `benchmarks/` still
has no such guard.
