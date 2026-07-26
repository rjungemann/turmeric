# `gc-heap-struct-rc` fixture fails on macOS/Darwin: heap-bytes probe reports 16320, not 0

**Severity:** low-medium (one fixture red on Darwin; either a real `rc<H>` `:heap`
leak or malloc-zone measurement noise -- needs confirming on glibc/CI).
**Not introduced here:** the committed `build/tur` reports the same 16320, and the
fixture has zero `#reads`/`#refine` forms, so the 2026-07-26 `#reads` entry-check
change cannot affect it. Pre-existing.

## Summary

`tests/fixtures/gc-heap-struct-rc` asserts no net malloc growth (`expected.stdout`
line 2 is `0`) for the acyclic `rc<H>` `:heap` drop-glue case. On this macOS box
it deterministically prints `16320` on line 2 (`diff`: `< 0 / > 16320`), so the
fixture is red under `tests/run.sh` (`2363 passed, 1 failed`).

## Reproduce

```sh
TUR=./build-debug/tur   # or the committed ./build/tur -- same result
diff tests/fixtures/gc-heap-struct-rc/expected.stdout \
     <(ASAN_OPTIONS=detect_leaks=0 $TUR run tests/fixtures/gc-heap-struct-rc/input.tur 2>/dev/null)
# 2c2 < 0 / > 16320   (deterministic across runs)
```

## Notes toward a root cause

`heap-bytes()` on Darwin uses `malloc_zone_statistics(malloc_default_zone(),
&ms).size_in_use`, which measures the **whole default zone**, not just this
fixture's allocations -- so 16320 may be zone-wide bookkeeping/noise from stdlib
init rather than a true leak in the `rc<H>` drop glue. The glibc path
(`mallinfo2().uordblks`) is narrower; the fixture comment says "CI covers glibc
and Darwin". Confirm on a glibc box: if glibc also shows non-zero growth it is a
real `:heap` `rc<T>` drop-glue leak (regression of
`docs/archive/gc-heap-struct-rc-not-a-control-block.md`); if only Darwin, the
Darwin probe needs to measure a fixture-scoped delta (baseline before / after)
rather than absolute `size_in_use`.
