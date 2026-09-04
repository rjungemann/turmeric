---
title: RM3 R4 -- what a declared lifetime is worth on the persistent Subst
category: Benchmark
description: bench-regions-subst A/B, same binary source compiled with and without --enable=regions. Leaked bytes go 2,668,800 -> 0, allocation count 82,785 -> 27,190, and peak RSS 7,340 -> 3,892 kB; time is 17-23% better for small substitutions and at parity from n=64 up. Checksums identical in every row.
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

The bracket sits inside `one-pass`, an ordinary non-main defn, which is both the
spelling a caller would write and the reason R4 had to route its region through
the CPS emitter as well as the direct one. An earlier revision of this benchmark
put every bracket in `main` to work around
`docs/archive/cps-direct-bt-scope-closure-temp-undeclared.md`; the numbers below
are from the natural shape, after that fix.

## Memory -- the headline

`valgrind --leak-check=summary`, one full sweep:

| | flag off | flag on |
|---|---|---|
| in use at exit | 2,668,928 B in 55,601 blocks | **65,856 B in 6 blocks** |
| definitely lost | 433,872 B in 9,039 blocks | **0** |
| indirectly lost | 2,234,928 B in 46,561 blocks | **0** |
| total allocations | 82,785 | **27,190** |
| bytes allocated | 4,784,216 | 2,181,144 |

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

**Peak RSS: 7,340 kB off, 3,892 kB on** -- 47% lower with the flag on, and
bit-stable across five runs of each arm. `/bin/true` measures 1,320 kB on the
same harness, so net of process baseline it is 6,020 kB against 2,572 kB.

### How that number was got wrong once, and how it is measured now

An earlier revision of this file reported the opposite -- "peak RSS is HIGHER
with the flag on, 2,076 vs 2,360 kB" -- and led with it. That was a measurement
artifact, not a result. It came from a shell loop polling `/proc/<pid>/status`
for `VmHWM` every 50 ms against a program that runs in 21 ms: most runs sampled
once or not at all, and repeating it produced 240, 1584, 2424, 2484 and 2500 kB
for the SAME binary. A poller cannot measure the peak of a program shorter than
its polling interval, and the spread said so plainly before anyone read the
numbers.

The figures above come from `wait4(2)`'s `ru_maxrss` -- the kernel's own
high-water mark for the child, sampled by nobody and exact by construction:

```c
pid_t p = fork();
if (p == 0) { execv(argv[1], argv + 1); _exit(127); }
int st; struct rusage ru;
wait4(p, &st, 0, &ru);
fprintf(stderr, "%ld\n", ru.ru_maxrss);   /* kB on Linux */
```

Five runs of each arm return the identical value. Use this, not a poller, for
any program that finishes in less than a second.

## Time -- median of three, ns per (bind + walk)

| n | off | on | on/off |
|---:|---:|---:|---:|
| 1 | 220.69 | 203.17 | 0.92x |
| 2 | 165.11 | 127.22 | 0.77x |
| 4 | 141.00 | 112.13 | 0.80x |
| 8 | 141.09 | 117.12 | 0.83x |
| 16 | 165.24 | 136.36 | 0.83x |
| 32 | 262.94 | 242.34 | 0.92x |
| 64 | 487.96 | 488.36 | 1.00x |
| 128 | 978.93 | 1034.52 | 1.06x |
| 256 | 2074.32 | 2135.68 | 1.03x |
| 512 | 4434.23 | 4454.66 | 1.00x |

Faster where the region is doing what a bump allocator does -- 17-23% for
2 <= n <= 32, where the cost per pass is dominated by malloc traffic that
becomes a pointer increment. Flat from n = 64 up, where the walk (O(n) per
lookup on an assoc list) dominates everything.

**Do not read the n >= 128 rows as a regression.** `passes-for` gives those rows
3-12 passes; a median of three runs of three passes does not separate 1.06x from
noise. The n <= 32 rows have 195-2000 passes each and are where the signal is.

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
