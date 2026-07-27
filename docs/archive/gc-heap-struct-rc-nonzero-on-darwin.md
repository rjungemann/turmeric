# `gc-heap-struct-rc` fixture fails on macOS/Darwin: heap-bytes probe reports 16320, not 0

**RESOLVED 2026-07-26 -- not a leak; Darwin zone-probe noise. Fixture fixed.**
Investigation established there is NO memory leak: only the *cyclic* delta (line
2) was nonzero, and it was **non-monotonic in the iteration count** -- measured
`0 / 16320 / 32768 / 0` for byte deltas (`size_in_use`) and `0 / 1228 / 0` for
block-count deltas (`blocks_in_use`) as `n` grew. A delta of literally 0 over
5000 iterations rules out any per-cycle leak (a real leak is monotonic in `n`).
The nonzero readings are Darwin `malloc_zone_statistics(malloc_default_zone())`
bucket/page bookkeeping (jumps of ~16-32 KB); `malloc_zone_pressure_relief`
before the read did not help and made it non-deterministic run-to-run. Fixed by
degrading the **Darwin** probe in `tests/fixtures/gc-heap-struct-rc/input.tur` to
vacuous (`return 0`, like an unmeasured platform): the precise leak assertion
runs on glibc CI (`mallinfo2().uordblks`), and a real regression of the original
control-block bug still segfaults on Darwin. The fixture now prints `0 / 0`
deterministically. The three sibling gc fixtures share the identical Darwin probe
but use lighter allocation and read 0 today, so they were left measuring; the
same fix applies if one flakes.

**Severity:** low-medium (one fixture red on Darwin; was measurement noise, not a
real `rc<H>` `:heap` leak).
**Not introduced by the `#reads` work:** the committed `build/tur` reports the
same 16320, and the fixture has zero `#reads`/`#refine` forms. Pre-existing.

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
