# Capture/restore cost curve (SX0(a))

Generated: 2026-08-22 17:37 at 18321cac by `benchmarks/run-capture-curve.sh`.

Compiler: `./build-release/tur`.  Host: x86_64, Intel(R) Xeon(R) Processor @ 2.10GHz, 4 core(s).


Cost per capture and per restore as a function of live state under the prompt.
The useful reading is the **intercept against the slope**: a strategy with a
large constant and no slope beats one with a small constant and a real slope
past some crossover, and that crossover is the design input. Every figure is
nanoseconds per operation, best of three timed rounds after an untimed warm-up.

Axes: `F` frames under the prompt, `E` owning-env bytes per frame (`rc` = a
refcount bump rather than a byte copy), `R` resumes per capture, `T` trailed
writes under the prompt (0 until SX1 exists).


## Fitted models

`ns = a + b*F + c*(F*E)` for the chain path, `a + b*F + c*E` for the rest.
The env term is per-FRAME on a chain -- `dk_copy_node` fires `env_clone` once
per owning node -- so `F*E` is the regressor; the script records what fitting
the plan's literal `c*E` did instead. `rc` rows are excluded from the `c` fit
(a refcount bump is a different mechanism, not one byte of a copy) and appear
in the per-path tables below.

| path | R | what | a (ns) | b (ns/frame) | c (ns/env byte/frame) | R^2 |
|---|---:|---|---:|---:|---:|---:|
| dk | 1 | capture | 27.9 | 17.311 | 0.1440 | 0.9639 |
| dk | 1 | restore | 36.9 | 26.041 | 0.1777 | 0.9597 |
| dk | 8 | capture | 28.1 | 16.584 | 0.1622 | 0.9602 |
| dk | 8 | restore | 41.1 | 25.838 | 0.1783 | 0.9629 |
| fiber | 1 | capture | 2513.5 | -0.291 | 0.0000 | 0.9498 |
| fiber | 1 | restore | 486.4 | -0.003 | 0.0000 | 0.2381 |
| fiber | 8 | capture | 2540.9 | -0.258 | 0.0000 | 0.9223 |
| fiber | 8 | restore | 472.0 | 0.001 | 0.0000 | 0.0702 |

A near-zero R^2 on the `fiber` rows is the RESULT, not a bad fit: there is no
trend in F for the regression to explain. Read those rows as "a = the cost,
b = 0" -- which is precisely the constant-cost, zero-slope strategy the
literature describes, measured rather than assumed.

`cloneable` is fitted separately, against E alone: it holds one env and no
chain, so F does not enter and `b` would be a regression against a constant.
Its slope in F is zero BY CONSTRUCTION, not by measurement.

| path | R | what | a (ns) | c (ns/env byte) | R^2 |
|---|---:|---|---:|---:|---:|
| cloneable | 1 | capture | 7.8 | 0.1486 | 0.6202 |
| cloneable | 1 | restore | 1.5 | -0.0028 | 0.0972 |
| cloneable | 8 | capture | 7.5 | 0.1261 | 0.5598 |
| cloneable | 8 | restore | 1.4 | 0.0083 | 0.9994 |

## Per-frame cost across the sweep (dk, E=0, R=1)

A single slope assumes the per-frame cost is constant. This column is how far
that assumption holds -- and where it stops. The cost settles into a flat
band from F=32 to F=2048 and then steps up sharply at F=4096: a chain that
size no longer fits in cache, and both copying and replaying it start paying
for misses. The fitted `b` is the flat band; past ~2048 frames the real cost
is worse than the model says.

| F | capture ns/frame | restore ns/frame |
|---:|---:|---:|
| 1 | 41.61 | 61.44 |
| 2 | 28.00 | 40.09 |
| 4 | 21.90 | 29.79 |
| 8 | 22.28 | 25.37 |
| 16 | 15.99 | 25.53 |
| 32 | 15.99 | 23.17 |
| 64 | 15.60 | 22.20 |
| 128 | 14.54 | 22.14 |
| 256 | 14.52 | 22.63 |
| 512 | 15.13 | 23.47 |
| 1024 | 15.15 | 23.58 |
| 2048 | 15.30 | 23.62 |
| 4096 | 18.28 | 26.77 |

## dk

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 1 | 41.6 | 61.44 | 360 |
| 1 | rc | 1 | 46.2 | 68.16 | 360 |
| 1 | 8 | 1 | 52.2 | 76.94 | 368 |
| 1 | 64 | 1 | 51.5 | 78.67 | 424 |
| 2 | 0 | 1 | 56.0 | 80.17 | 480 |
| 2 | rc | 1 | 59.5 | 89.58 | 480 |
| 2 | 8 | 1 | 86.4 | 112.81 | 496 |
| 2 | 64 | 1 | 80.9 | 109.60 | 608 |
| 4 | 0 | 1 | 87.6 | 119.14 | 720 |
| 4 | rc | 1 | 92.7 | 132.58 | 720 |
| 4 | 8 | 1 | 120.8 | 168.66 | 752 |
| 4 | 64 | 1 | 126.5 | 166.77 | 976 |
| 8 | 0 | 1 | 178.2 | 202.96 | 1200 |
| 8 | rc | 1 | 142.7 | 213.63 | 1200 |
| 8 | 8 | 1 | 213.9 | 308.78 | 1264 |
| 8 | 64 | 1 | 217.2 | 304.64 | 1712 |
| 16 | 0 | 1 | 255.8 | 408.56 | 2160 |
| 16 | rc | 1 | 295.9 | 422.60 | 2160 |
| 16 | 8 | 1 | 426.8 | 614.54 | 2288 |
| 16 | 64 | 1 | 462.6 | 572.08 | 3184 |
| 32 | 0 | 1 | 511.8 | 741.38 | 4080 |
| 32 | rc | 1 | 535.2 | 783.47 | 4080 |
| 32 | 8 | 1 | 976.4 | 1228.00 | 4336 |
| 32 | 64 | 1 | 1042.1 | 1188.17 | 6128 |
| 64 | 0 | 1 | 998.6 | 1420.50 | 7920 |
| 64 | rc | 1 | 1090.8 | 1483.23 | 7920 |
| 64 | 8 | 1 | 1428.6 | 2367.31 | 8432 |
| 64 | 64 | 1 | 1666.6 | 2268.09 | 12016 |
| 128 | 0 | 1 | 1861.0 | 2833.82 | 15600 |
| 128 | rc | 1 | 2190.1 | 2904.03 | 15600 |
| 128 | 8 | 1 | 3113.3 | 4751.47 | 16624 |
| 128 | 64 | 1 | 3468.5 | 4658.86 | 23792 |
| 256 | 0 | 1 | 3718.1 | 5793.64 | 30960 |
| 256 | rc | 1 | 4147.1 | 6337.66 | 30960 |
| 256 | 8 | 1 | 6040.5 | 9922.18 | 33008 |
| 256 | 64 | 1 | 6834.6 | 9929.54 | 47344 |
| 512 | 0 | 1 | 7747.3 | 12018.54 | 61680 |
| 512 | rc | 1 | 8703.4 | 13254.23 | 61680 |
| 512 | 8 | 1 | 11981.4 | 19508.83 | 65776 |
| 512 | 64 | 1 | 12962.3 | 19311.82 | 94448 |
| 1024 | 0 | 1 | 15510.9 | 24150.23 | 123120 |
| 1024 | rc | 1 | 14997.7 | 25408.82 | 123120 |
| 1024 | 8 | 1 | 23910.7 | 38539.53 | 131312 |
| 1024 | 64 | 1 | 23747.2 | 38086.60 | 188656 |
| 2048 | 0 | 1 | 31327.7 | 48379.14 | 246000 |
| 2048 | rc | 1 | 30523.6 | 54366.42 | 246000 |
| 2048 | 8 | 1 | 47636.0 | 78451.17 | 262384 |
| 2048 | 64 | 1 | 49279.8 | 77504.34 | 377072 |
| 4096 | 0 | 1 | 74890.4 | 109656.61 | 491760 |
| 4096 | rc | 1 | 94131.5 | 119275.59 | 491760 |
| 4096 | 8 | 1 | 103125.8 | 166385.05 | 524528 |
| 4096 | 64 | 1 | 113822.3 | 165494.11 | 753904 |
| 1 | 0 | 8 | 40.4 | 64.08 | 360 |
| 1 | rc | 8 | 40.7 | 65.72 | 360 |
| 1 | 8 | 8 | 53.3 | 76.73 | 368 |
| 1 | 64 | 8 | 51.6 | 79.52 | 424 |
| 2 | 0 | 8 | 52.9 | 93.54 | 480 |
| 2 | rc | 8 | 55.9 | 92.62 | 480 |
| 2 | 8 | 8 | 78.5 | 111.61 | 496 |
| 2 | 64 | 8 | 111.7 | 110.36 | 608 |
| 4 | 0 | 8 | 79.1 | 134.08 | 720 |
| 4 | rc | 8 | 86.7 | 131.48 | 720 |
| 4 | 8 | 8 | 121.9 | 170.76 | 752 |
| 4 | 64 | 8 | 140.9 | 167.09 | 976 |
| 8 | 0 | 8 | 139.8 | 203.92 | 1200 |
| 8 | rc | 8 | 149.6 | 216.22 | 1200 |
| 8 | 8 | 8 | 217.3 | 304.48 | 1264 |
| 8 | 64 | 8 | 287.4 | 308.25 | 1712 |
| 16 | 0 | 8 | 257.6 | 406.19 | 2160 |
| 16 | rc | 8 | 272.6 | 405.96 | 2160 |
| 16 | 8 | 8 | 446.2 | 593.02 | 2288 |
| 16 | 64 | 8 | 463.3 | 577.19 | 3184 |
| 32 | 0 | 8 | 500.3 | 770.87 | 4080 |
| 32 | rc | 8 | 595.8 | 790.29 | 4080 |
| 32 | 8 | 8 | 846.2 | 1194.53 | 4336 |
| 32 | 64 | 8 | 890.1 | 1164.69 | 6128 |
| 64 | 0 | 8 | 914.4 | 1405.14 | 7920 |
| 64 | rc | 8 | 989.5 | 1495.80 | 7920 |
| 64 | 8 | 8 | 1401.0 | 2371.74 | 8432 |
| 64 | 64 | 8 | 1590.6 | 2291.89 | 12016 |
| 128 | 0 | 8 | 1744.2 | 2745.13 | 15600 |
| 128 | rc | 8 | 2040.4 | 2947.83 | 15600 |
| 128 | 8 | 8 | 2870.0 | 4751.88 | 16624 |
| 128 | 64 | 8 | 3143.2 | 4820.29 | 23792 |
| 256 | 0 | 8 | 3837.0 | 5829.24 | 30960 |
| 256 | rc | 8 | 4199.6 | 6195.22 | 30960 |
| 256 | 8 | 8 | 5800.2 | 9696.90 | 33008 |
| 256 | 64 | 8 | 6199.8 | 9966.94 | 47344 |
| 512 | 0 | 8 | 7658.0 | 11766.46 | 61680 |
| 512 | rc | 8 | 8229.7 | 13039.51 | 61680 |
| 512 | 8 | 8 | 12659.9 | 19897.80 | 65776 |
| 512 | 64 | 8 | 12961.4 | 19393.78 | 94448 |
| 1024 | 0 | 8 | 15175.5 | 23960.64 | 123120 |
| 1024 | rc | 8 | 17207.3 | 26489.48 | 123120 |
| 1024 | 8 | 8 | 23645.8 | 39859.72 | 131312 |
| 1024 | 64 | 8 | 24843.5 | 39378.49 | 188656 |
| 2048 | 0 | 8 | 30805.6 | 48687.83 | 246000 |
| 2048 | rc | 8 | 34301.2 | 52400.91 | 246000 |
| 2048 | 8 | 8 | 46117.1 | 75540.72 | 262384 |
| 2048 | 64 | 8 | 54813.8 | 76296.28 | 377072 |
| 4096 | 0 | 8 | 86085.6 | 98310.05 | 491760 |
| 4096 | rc | 8 | 81910.8 | 111187.49 | 491760 |
| 4096 | 8 | 8 | 93694.8 | 160007.48 | 524528 |
| 4096 | 64 | 8 | 106833.0 | 160917.62 | 753904 |

## fiber

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 1 | 2679.0 | 466.57 | 4194304 |
| 2 | 0 | 1 | 2402.7 | 486.52 | 4194304 |
| 4 | 0 | 1 | 2469.1 | 480.00 | 4194304 |
| 8 | 0 | 1 | 2593.1 | 490.62 | 4194304 |
| 16 | 0 | 1 | 2424.2 | 494.97 | 4194304 |
| 32 | 0 | 1 | 2411.8 | 489.60 | 4194304 |
| 64 | 0 | 1 | 2444.4 | 483.52 | 4194304 |
| 128 | 0 | 1 | 2431.6 | 486.59 | 4194304 |
| 256 | 0 | 1 | 2381.8 | 488.75 | 4194304 |
| 512 | 0 | 1 | 2327.7 | 492.33 | 4194304 |
| 1024 | 0 | 1 | 2414.5 | 489.67 | 4194304 |
| 2048 | 0 | 1 | 2164.8 | 478.96 | 4194304 |
| 4096 | 0 | 1 | 1267.7 | 469.73 | 4194304 |
| 1 | 0 | 8 | 2375.8 | 469.24 | 4194304 |
| 2 | 0 | 8 | 2570.4 | 471.31 | 4194304 |
| 4 | 0 | 8 | 2426.1 | 471.25 | 4194304 |
| 8 | 0 | 8 | 2412.7 | 473.37 | 4194304 |
| 16 | 0 | 8 | 2532.8 | 462.82 | 4194304 |
| 32 | 0 | 8 | 2476.8 | 478.38 | 4194304 |
| 64 | 0 | 8 | 2575.5 | 471.41 | 4194304 |
| 128 | 0 | 8 | 2444.9 | 465.25 | 4194304 |
| 256 | 0 | 8 | 2583.8 | 475.55 | 4194304 |
| 512 | 0 | 8 | 2489.4 | 480.16 | 4194304 |
| 1024 | 0 | 8 | 2544.4 | 477.23 | 4194304 |
| 2048 | 0 | 8 | 2208.4 | 477.39 | 4194304 |
| 4096 | 0 | 8 | 1417.0 | 473.24 | 4194304 |

## cloneable

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 1 | 6.8 | 1.34 | 32 |
| 0 | rc | 1 | 7.2 | 1.33 | 32 |
| 0 | 8 | 1 | 14.7 | 2.06 | 40 |
| 0 | 64 | 1 | 16.5 | 1.33 | 96 |
| 0 | 0 | 8 | 6.5 | 1.43 | 32 |
| 0 | rc | 8 | 7.4 | 1.83 | 32 |
| 0 | 8 | 8 | 14.4 | 1.49 | 40 |
| 0 | 64 | 8 | 14.8 | 1.96 | 96 |

## The T axis -- trailed writes (SX1)

`bt-mark`, T trailed writes, `bt-undo-to!`. This is the alternative to
capturing state at all: instead of copying a continuation so a second entry
does not see the first one's mutations, record the mutations and put them
back.

| T | cycle ns | ns per write | bytes on the trail |
|---:|---:|---:|---:|
| 1 | 8.2 | 8.15 | 32 |
| 2 | 13.1 | 6.56 | 64 |
| 4 | 21.4 | 5.35 | 128 |
| 8 | 39.0 | 4.87 | 256 |
| 16 | 74.6 | 4.66 | 512 |
| 32 | 154.8 | 4.84 | 1024 |
| 64 | 302.5 | 4.73 | 2048 |
| 128 | 588.4 | 4.60 | 4096 |
| 256 | 1179.6 | 4.61 | 8192 |
| 512 | 2365.5 | 4.62 | 16384 |
| 1024 | 5374.5 | 5.25 | 32768 |
| 2048 | 11591.3 | 5.66 | 65536 |
| 4096 | 23277.7 | 5.68 | 131072 |

A trailed write and its undo cost **5.0 ns** (mean over T >= 8), against
**22.8 ns per frame** to replay a DK chain slice. Undoing recorded state is
roughly 5x cheaper per unit of live state than restoring captured control --
and it is the mechanism that can be asymmetric, keeping what the search
learned while discarding what it assumed. That asymmetry, not the constant,
is why the plan routes solver backtracking through state rather than control.

## Baselines

| baseline | ns/op |
|---|---:|
| loop-iteration | 0.34 |
| plain-call | 1.96 |
| closure-call | 2.05 |
| hamt-snapshot | 596.51 |

## The crossover

Fitted on the `E = 0` rows only -- no owning envs on either side, so this is
the two mechanisms compared and nothing else.

- **capture**: the DK chain costs `27 + 15.3*F` ns (R^2 0.993); a fiber costs a flat `2514` ns (measured slope -0.291 ns/frame, R^2 0.950). They cross at **F = 163 frames** -- below that the chain slice is cheaper, above it the fiber's constant-cost switch wins.
- **restore**: the DK chain costs `35 + 22.8*F` ns (R^2 0.995); a fiber costs a flat `486` ns (measured slope -0.003 ns/frame, R^2 0.238). They cross at **F = 20 frames** -- below that the chain slice is cheaper, above it the fiber's constant-cost switch wins.

## Notes

- **Interpreter leg:** requested and declined. `tur --interpret` on this
  benchmark reports:

  > tur: eval: inline-C not supported in interpreter mode (function uses a native C implementation; run it with `tur build`/`tur run` instead of `--interpret`)

  Every timed region here is inline C, by design -- the question is what the
  runtime mechanism costs, not what a driver costs to reach it -- so the
  tree-walker has nothing it can run. The plan's interpreter question (is the
  tree-walker adequate for a thousands-per-second search layer?) needs a
  Turmeric-level search benchmark instead; SX2's `bench-logic-query` and
  `bench-backtrack-n-queens` are exactly that, and are where that row belongs.
- Run metadata: `closure_checksum=1000000 expected=1000000`
- Run metadata: `peak_rss_kb=91156`
