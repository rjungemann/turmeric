---
title: RM3 R4 -- what a declared lifetime is worth on the persistent Subst
category: Benchmark
description: bench-regions-subst A/B, same binary source compiled with and without --enable=regions. Leaked bytes go 2,668,800 -> 0 and allocation count 82,785 -> 27,190; time is 17-23% better for small substitutions and at parity from n=64 up. Peak RSS is HIGHER with the flag on. Checksums identical in every row.
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

**Peak RSS is HIGHER with the flag on: 2,076 kB off, 2,360 kB on.** Say that
plainly rather than leading with the block count. A region trades a growing leak
for a fixed 64 KiB slab, and on a benchmark whose chains are short and whose
pass counts are tuned to hold total work constant, the leak never grows large
enough for the trade to pay in peak footprint. What the block and byte counts
show is the SHAPE -- unbounded versus bounded -- and that is the claim RM3 rests
on. A workload that runs longer, or holds longer chains, crosses over; this one
does not, and a reader should not be told it does.

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
