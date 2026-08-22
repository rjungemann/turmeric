# Capture/restore cost curve (SX0(a))

Generated: 2026-08-22 15:52 at 10f20520 by `benchmarks/run-capture-curve.sh`.

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
| dk | 1 | capture | 22.6 | 16.959 | 0.2789 | 0.9335 |
| dk | 1 | restore | 35.3 | 22.357 | 0.1732 | 0.9600 |
| dk | 8 | capture | 22.0 | 15.944 | 0.2526 | 0.9448 |
| dk | 8 | restore | 36.5 | 22.555 | 0.1679 | 0.9620 |
| fiber | 1 | capture | 2056.4 | -0.236 | 0.0000 | 0.9308 |
| fiber | 1 | restore | 428.8 | -0.002 | 0.0000 | 0.8053 |
| fiber | 8 | capture | 2228.9 | -0.235 | 0.0000 | 0.8927 |
| fiber | 8 | restore | 425.2 | -0.000 | 0.0000 | 0.0624 |

A near-zero R^2 on the `fiber` rows is the RESULT, not a bad fit: there is no
trend in F for the regression to explain. Read those rows as "a = the cost,
b = 0" -- which is precisely the constant-cost, zero-slope strategy the
literature describes, measured rather than assumed.

`cloneable` is fitted separately, against E alone: it holds one env and no
chain, so F does not enter and `b` would be a regression against a constant.
Its slope in F is zero BY CONSTRUCTION, not by measurement.

| path | R | what | a (ns) | c (ns/env byte) | R^2 |
|---|---:|---|---:|---:|---:|
| cloneable | 1 | capture | 8.5 | 0.0813 | 0.5283 |
| cloneable | 1 | restore | 1.8 | 0.0005 | 0.3725 |
| cloneable | 8 | capture | 8.6 | 0.0924 | 0.5725 |
| cloneable | 8 | restore | 1.9 | -0.0003 | 0.1433 |

## Per-frame cost across the sweep (dk, E=0, R=1)

A single slope assumes the per-frame cost is constant. This column is how far
that assumption holds -- and where it stops. The cost settles into a flat
band from F=32 to F=2048 and then steps up sharply at F=4096: a chain that
size no longer fits in cache, and both copying and replaying it start paying
for misses. The fitted `b` is the flat band; past ~2048 frames the real cost
is worse than the model says.

| F | capture ns/frame | restore ns/frame |
|---:|---:|---:|
| 1 | 41.12 | 56.16 |
| 2 | 23.62 | 35.90 |
| 4 | 18.45 | 30.48 |
| 8 | 18.53 | 23.49 |
| 16 | 17.31 | 21.93 |
| 32 | 14.29 | 19.75 |
| 64 | 14.77 | 19.39 |
| 128 | 13.02 | 18.56 |
| 256 | 13.44 | 19.82 |
| 512 | 17.44 | 19.62 |
| 1024 | 16.13 | 19.42 |
| 2048 | 14.78 | 20.22 |
| 4096 | 27.34 | 21.83 |

## dk

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 1 | 41.1 | 56.16 | 360 |
| 1 | rc | 1 | 48.3 | 75.89 | 360 |
| 1 | 8 | 1 | 48.8 | 68.23 | 368 |
| 1 | 64 | 1 | 53.2 | 67.64 | 424 |
| 2 | 0 | 1 | 47.2 | 71.80 | 480 |
| 2 | rc | 1 | 55.7 | 79.58 | 480 |
| 2 | 8 | 1 | 86.2 | 98.55 | 496 |
| 2 | 64 | 1 | 89.9 | 98.87 | 608 |
| 4 | 0 | 1 | 73.8 | 121.93 | 720 |
| 4 | rc | 1 | 79.4 | 116.36 | 720 |
| 4 | 8 | 1 | 125.2 | 152.21 | 752 |
| 4 | 64 | 1 | 143.3 | 150.81 | 976 |
| 8 | 0 | 1 | 148.3 | 187.91 | 1200 |
| 8 | rc | 1 | 124.1 | 197.18 | 1200 |
| 8 | 8 | 1 | 234.8 | 279.91 | 1264 |
| 8 | 64 | 1 | 290.1 | 270.54 | 1712 |
| 16 | 0 | 1 | 277.0 | 350.95 | 2160 |
| 16 | rc | 1 | 265.3 | 371.15 | 2160 |
| 16 | 8 | 1 | 569.8 | 518.69 | 2288 |
| 16 | 64 | 1 | 639.0 | 506.01 | 3184 |
| 32 | 0 | 1 | 457.3 | 631.93 | 4080 |
| 32 | rc | 1 | 554.4 | 724.45 | 4080 |
| 32 | 8 | 1 | 979.2 | 1022.95 | 4336 |
| 32 | 64 | 1 | 1606.7 | 1021.43 | 6128 |
| 64 | 0 | 1 | 945.4 | 1241.26 | 7920 |
| 64 | rc | 1 | 941.0 | 1358.01 | 7920 |
| 64 | 8 | 1 | 1415.5 | 2017.96 | 8432 |
| 64 | 64 | 1 | 2953.5 | 2075.43 | 12016 |
| 128 | 0 | 1 | 1666.5 | 2375.85 | 15600 |
| 128 | rc | 1 | 1713.0 | 2592.46 | 15600 |
| 128 | 8 | 1 | 3744.5 | 4043.18 | 16624 |
| 128 | 64 | 1 | 4001.6 | 4136.80 | 23792 |
| 256 | 0 | 1 | 3441.2 | 5072.91 | 30960 |
| 256 | rc | 1 | 3419.0 | 5600.99 | 30960 |
| 256 | 8 | 1 | 6210.0 | 8465.53 | 33008 |
| 256 | 64 | 1 | 6722.2 | 8836.65 | 47344 |
| 512 | 0 | 1 | 8931.7 | 10047.52 | 61680 |
| 512 | rc | 1 | 7144.5 | 11347.57 | 61680 |
| 512 | 8 | 1 | 16086.9 | 16744.98 | 65776 |
| 512 | 64 | 1 | 16795.2 | 17144.45 | 94448 |
| 1024 | 0 | 1 | 16514.0 | 19884.02 | 123120 |
| 1024 | rc | 1 | 14410.9 | 23270.91 | 123120 |
| 1024 | 8 | 1 | 24009.8 | 32336.53 | 131312 |
| 1024 | 64 | 1 | 34323.7 | 33643.28 | 188656 |
| 2048 | 0 | 1 | 30269.7 | 41409.50 | 246000 |
| 2048 | rc | 1 | 27590.8 | 46842.39 | 246000 |
| 2048 | 8 | 1 | 45265.6 | 67201.51 | 262384 |
| 2048 | 64 | 1 | 63049.0 | 72414.20 | 377072 |
| 4096 | 0 | 1 | 111993.1 | 89410.30 | 491760 |
| 4096 | rc | 1 | 103161.3 | 106300.80 | 491760 |
| 4096 | 8 | 1 | 117658.8 | 158393.87 | 524528 |
| 4096 | 64 | 1 | 163538.4 | 169531.50 | 753904 |
| 1 | 0 | 8 | 34.1 | 55.28 | 360 |
| 1 | rc | 8 | 36.7 | 72.48 | 360 |
| 1 | 8 | 8 | 56.8 | 67.17 | 368 |
| 1 | 64 | 8 | 53.8 | 67.33 | 424 |
| 2 | 0 | 8 | 48.5 | 83.65 | 480 |
| 2 | rc | 8 | 48.4 | 78.82 | 480 |
| 2 | 8 | 8 | 81.1 | 96.86 | 496 |
| 2 | 64 | 8 | 90.1 | 97.05 | 608 |
| 4 | 0 | 8 | 69.6 | 131.32 | 720 |
| 4 | rc | 8 | 73.3 | 114.56 | 720 |
| 4 | 8 | 8 | 114.1 | 151.83 | 752 |
| 4 | 64 | 8 | 132.7 | 152.09 | 976 |
| 8 | 0 | 8 | 121.1 | 185.77 | 1200 |
| 8 | rc | 8 | 125.2 | 192.36 | 1200 |
| 8 | 8 | 8 | 208.8 | 266.37 | 1264 |
| 8 | 64 | 8 | 280.7 | 267.21 | 1712 |
| 16 | 0 | 8 | 231.5 | 364.88 | 2160 |
| 16 | rc | 8 | 244.9 | 366.71 | 2160 |
| 16 | 8 | 8 | 493.0 | 534.70 | 2288 |
| 16 | 64 | 8 | 753.7 | 531.63 | 3184 |
| 32 | 0 | 8 | 486.5 | 654.54 | 4080 |
| 32 | rc | 8 | 496.8 | 684.51 | 4080 |
| 32 | 8 | 8 | 753.6 | 1040.49 | 4336 |
| 32 | 64 | 8 | 1196.1 | 1019.95 | 6128 |
| 64 | 0 | 8 | 859.5 | 1234.02 | 7920 |
| 64 | rc | 8 | 929.0 | 1335.10 | 7920 |
| 64 | 8 | 8 | 1756.9 | 2079.93 | 8432 |
| 64 | 64 | 8 | 1729.3 | 2066.59 | 12016 |
| 128 | 0 | 8 | 1611.3 | 2416.45 | 15600 |
| 128 | rc | 8 | 1639.7 | 2684.32 | 15600 |
| 128 | 8 | 8 | 2962.4 | 4068.79 | 16624 |
| 128 | 64 | 8 | 3435.0 | 4218.73 | 23792 |
| 256 | 0 | 8 | 3837.6 | 5106.32 | 30960 |
| 256 | rc | 8 | 4094.8 | 5700.97 | 30960 |
| 256 | 8 | 8 | 6472.8 | 8463.02 | 33008 |
| 256 | 64 | 8 | 7645.7 | 8583.76 | 47344 |
| 512 | 0 | 8 | 6951.5 | 10125.42 | 61680 |
| 512 | rc | 8 | 6903.3 | 12125.85 | 61680 |
| 512 | 8 | 8 | 12585.8 | 16992.00 | 65776 |
| 512 | 64 | 8 | 14922.3 | 17234.67 | 94448 |
| 1024 | 0 | 8 | 16230.2 | 20172.71 | 123120 |
| 1024 | rc | 8 | 20281.5 | 22874.93 | 123120 |
| 1024 | 8 | 8 | 24203.5 | 33211.00 | 131312 |
| 1024 | 64 | 8 | 31640.9 | 34054.24 | 188656 |
| 2048 | 0 | 8 | 30700.6 | 39936.61 | 246000 |
| 2048 | rc | 8 | 34309.8 | 46913.67 | 246000 |
| 2048 | 8 | 8 | 48453.4 | 67657.53 | 262384 |
| 2048 | 64 | 8 | 62002.1 | 72843.10 | 377072 |
| 4096 | 0 | 8 | 84177.3 | 86538.65 | 491760 |
| 4096 | rc | 8 | 87503.7 | 102123.75 | 491760 |
| 4096 | 8 | 8 | 96239.4 | 153598.24 | 524528 |
| 4096 | 64 | 8 | 146653.0 | 155103.37 | 753904 |

## fiber

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 1 | 2406.0 | 428.48 | 4194304 |
| 2 | 0 | 1 | 2070.5 | 427.66 | 4194304 |
| 4 | 0 | 1 | 1892.5 | 429.98 | 4194304 |
| 8 | 0 | 1 | 2089.9 | 427.61 | 4194304 |
| 16 | 0 | 1 | 2047.4 | 430.13 | 4194304 |
| 32 | 0 | 1 | 2022.2 | 430.34 | 4194304 |
| 64 | 0 | 1 | 1975.2 | 428.23 | 4194304 |
| 128 | 0 | 1 | 1956.7 | 428.08 | 4194304 |
| 256 | 0 | 1 | 1882.5 | 428.52 | 4194304 |
| 512 | 0 | 1 | 1950.2 | 427.23 | 4194304 |
| 1024 | 0 | 1 | 1905.1 | 428.03 | 4194304 |
| 2048 | 0 | 1 | 1708.4 | 424.86 | 4194304 |
| 4096 | 0 | 1 | 1057.6 | 422.50 | 4194304 |
| 1 | 0 | 8 | 1989.8 | 424.33 | 4194304 |
| 2 | 0 | 8 | 2192.4 | 422.87 | 4194304 |
| 4 | 0 | 8 | 2145.0 | 426.27 | 4194304 |
| 8 | 0 | 8 | 2212.0 | 426.64 | 4194304 |
| 16 | 0 | 8 | 2145.8 | 427.14 | 4194304 |
| 32 | 0 | 8 | 2178.0 | 426.31 | 4194304 |
| 64 | 0 | 8 | 2517.8 | 424.72 | 4194304 |
| 128 | 0 | 8 | 2173.9 | 424.08 | 4194304 |
| 256 | 0 | 8 | 2176.5 | 424.97 | 4194304 |
| 512 | 0 | 8 | 2188.3 | 424.90 | 4194304 |
| 1024 | 0 | 8 | 2161.6 | 424.15 | 4194304 |
| 2048 | 0 | 8 | 1977.8 | 424.44 | 4194304 |
| 4096 | 0 | 8 | 1208.2 | 424.49 | 4194304 |

## cloneable

| F | E | R | capture ns | restore ns | bytes/capture |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 1 | 7.4 | 1.77 | 32 |
| 0 | rc | 1 | 6.3 | 1.82 | 32 |
| 0 | 8 | 1 | 12.9 | 1.82 | 40 |
| 0 | 64 | 1 | 13.2 | 1.82 | 96 |
| 0 | 0 | 8 | 7.6 | 1.86 | 32 |
| 0 | rc | 8 | 6.2 | 1.86 | 32 |
| 0 | 8 | 8 | 13.1 | 1.91 | 40 |
| 0 | 64 | 8 | 14.0 | 1.86 | 96 |

## Baselines

| baseline | ns/op |
|---|---:|
| loop-iteration | 2.29 |
| plain-call | 1.53 |
| closure-call | 2.16 |
| hamt-snapshot | 526.12 |

## The crossover

Fitted on the `E = 0` rows only -- no owning envs on either side, so this is
the two mechanisms compared and nothing else.

- **capture**: the DK chain costs `21 + 14.9*F` ns (R^2 0.964); a fiber costs a flat `2056` ns (measured slope -0.236 ns/frame, R^2 0.931). They cross at **F = 137 frames** -- below that the chain slice is cheaper, above it the fiber's constant-cost switch wins.
- **restore**: the DK chain costs `36 + 19.5*F` ns (R^2 0.997); a fiber costs a flat `429` ns (measured slope -0.002 ns/frame, R^2 0.805). They cross at **F = 20 frames** -- below that the chain slice is cheaper, above it the fiber's constant-cost switch wins.

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
