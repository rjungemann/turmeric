---
title: RM3 R4 -- what a declared lifetime is worth on the persistent Subst
category: Benchmark
description: bench-regions-subst A/B, same binary source compiled with and without --enable=regions. Leaked bytes go 2,668,800 -> 0 and allocation count 64,645 -> 9,050; wall time is 20-25% better for small substitutions and at parity from n=64 up. Checksums identical in every row.
---

# bench-regions-subst -- `--enable=regions` A/B

Measured 2026-09-04. Release compiler (`build-release/tur`), same source both
arms (`benchmarks/bench-regions-subst.tur`); the only difference is the flag,
so this is the region and not a rewritten program.

The workload is bench-logic-subst's PERSISTENT path: build an n-link `Subst`,
walk every variable once, drop it, repeat. `passes-for` holds total work roughly
constant across n. Each pass is bracketed in one `bt-scope`, which R4 makes a
region boundary; the bracket's result is the walk sum, an `:int`, so the static
escape check clears the rewind.

## Memory -- the headline

`valgrind --leak-check=summary`, one full sweep:

| | flag off | flag on |
|---|---|---|
| in use at exit | 2,668,928 B in 55,601 blocks | **65,856 B in 6 blocks** |
| definitely lost | 433,872 B in 9,039 blocks | **0** |
| indirectly lost | 2,234,928 B in 46,561 blocks | **0** |
| total allocations | 64,645 | **9,050** |
| bytes allocated | 2,890,088 | 287,016 |

The 55,595 allocations that disappear are the per-link spine boxes. They are
the RM1 leak sweep's largest real category and the thing RM2 could not own; here
they are reclaimed by generation and the leak is not reduced but **eliminated**
on this workload.

The 65,856 bytes the ON arm still holds at exit are ONE 64 KiB region slab
(`TUR_REGION_SLAB`) plus its bookkeeping, kept in the pool for reuse. That is
O(1) in the number of passes, not O(passes), and valgrind reports it as still
reachable rather than lost. It is not returned at process exit because nothing
in an emitted program calls `tur_region_shutdown` -- an honest cost to state,
not a leak, and worth closing before graduation.

Peak RSS over the whole sweep: **2,008 kB off, 1,820 kB on.** Small, because
this benchmark's chains are short and its pass counts are tuned to hold work
constant; the block counts above are the shape that matters.

## Time -- median of three, ns per (bind + walk)

| n | off | on | on/off |
|---:|---:|---:|---:|
| 1 | 135.17 | 106.77 | 0.79x |
| 2 | 126.30 | 94.49 | 0.75x |
| 4 | 121.03 | 90.41 | 0.75x |
| 8 | 127.21 | 102.07 | 0.80x |
| 16 | 174.49 | 131.88 | 0.76x |
| 32 | 289.10 | 251.43 | 0.87x |
| 64 | 549.71 | 532.40 | 0.97x |
| 128 | 1097.01 | 1031.66 | 0.94x |
| 256 | 2103.18 | 2409.53 | 1.15x |
| 512 | 4824.98 | 4935.32 | 1.02x |

Faster where the region is doing what a bump allocator does -- 20-25% for
n <= 16, where the cost per pass is dominated by malloc traffic that becomes a
pointer increment. Converging to parity from n = 64 up, where the walk (O(n) per
lookup on an assoc list) dominates everything.

**Do not read n = 256 as a regression.** `passes-for` gives those rows THREE
passes; a median of three runs of three passes is not a measurement that
separates 1.15x from noise. The n <= 32 rows have 195-2000 passes each and are
where the signal is.

Wall clock for the whole sweep: 23 ms off, 21 ms on (three runs each, spread
<= 1 ms).

## Correctness

The `checksum` column is printed per row precisely so a rewind that reclaimed
something still live would show as a wrong number rather than as a faster run.
**Every row's checksum is identical in both arms.** `tests/run-regions-seam.sh`
asserts the same property across eleven fixtures.

## Reproducing

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-release -j
./build-release/tur build                 benchmarks/bench-regions-subst.tur -o /tmp/b-off
./build-release/tur build --enable=regions benchmarks/bench-regions-subst.tur -o /tmp/b-on
valgrind --leak-check=summary /tmp/b-off
valgrind --leak-check=summary /tmp/b-on
```
