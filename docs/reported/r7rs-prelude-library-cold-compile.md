# `#lang r7rs`: the first build compiles the whole stdlib (8.5 s)

**Severity:** low. The prelude split
(docs/archive/r7rs-programs-compile-slowly.md) makes every build after the
first about 1 s, but the first build with an empty cache now takes 8.5 s for
a one-line program with a Release `tur`. As one unit it took 3.0 s. The first
build happens once per `tur` version, `cc` and flags. The cache lives in
`<tmpdir>`, so a machine that clears it on reboot pays again, and so does
every fresh CI runner.

## Repro

```sh
d=$(mktemp -d)
time TMPDIR=$d ./build-release/tur build tests/fixtures/r7rs-named-let-sum/input.tur -o /tmp/x   # 8.5 s
time TMPDIR=$d ./build-release/tur build tests/fixtures/r7rs-named-let-sum/input.tur -o /tmp/x   # 0.95 s
```

## Root cause

The library unit gives every stdlib definition external linkage, so that any
program can link against it. In one unit, cc dropped the ~1,100 of the
~1,780 functions a program never reaches before optimizing anything. In the
library it has to compile all of them at `-O2`. That is about 7.5 s of `cc`
over 1.44 MB of C (`src/main.c`, `prelude_split_object`).

## Measured (2026-09-28): what a lower level buys and costs

The library unit's text (1.44 MB, program-independent), compiled on its own
with gcc 13.3 on a 4-core Linux box. The run time is the best of seven runs
of a benchmark that mostly runs prelude code: `fib 27` through the generic
`+`, building and walking 400,000-element lists and vectors, `string-append`
in a loop, and `reverse`. It was linked against each object, with the same
program unit:

| library flags | compile | run | vs `-O2` |
| --- | --- | --- | --- |
| `-O2` (today) | 7.8 s | 0.40-0.41 s | -- |
| `-O1 -foptimize-sibling-calls` | 3.8 s | 0.43-0.46 s | +9-12% |
| `-Os` | 5.5 s | 0.46 s | +16% |
| `-O2 -fno-inline-functions -fno-inline-small-functions` | 6.1 s | 0.47 s | +17% |
| `-O2 -fno-inline` | 5.7 s | 0.49 s | +22% |

`-O1 -foptimize-sibling-calls` is the only middle setting worth having. It
beats the others on both axes, and the explicit flag keeps the sibling calls
that a CPS tail call needs (cps-self-tail-call-relies-on-sibling-call). It
still costs about a tenth of the run time of prelude-heavy code, for the
life of the program, to save about 4 s once per `tur` version, `cc` and
flags. That trade is the maintainer's to make, so the default is unchanged.
The other two directions keep `-O2`: compiling in parallel pieces, or
warming the cache ahead of time.

One hazard for the parallel-pieces direction: a `static` function is
compiled into every piece that calls it. If it holds a function-local
`static`, that state forks. `tests/check-r7rs-prelude-split.sh`'s
defined-in-both check would need to run across the pieces too.

## Fix directions

- **Compile the library in parallel pieces.** Split the library text at
  function boundaries into N files, each with the shared preamble
  (`emit_split_state` already knows where the boundaries are). Compile them
  concurrently and link all N objects. This costs more CPU but brings wall
  time down near 7.5 s / N.
- **Warm the cache when `tur` is built or installed.** A post-build step
  that builds a trivial `#lang r7rs` program would do it. The cache then has
  to live somewhere that survives a reboot, such as next to the `tur` binary
  or in the user's cache directory, rather than in `<tmpdir>`.
- **Compile the library at a lower level.** The split already stops cc from
  inlining the prelude into the program, so the objection the original
  report raised against `-O1` for the prelude no longer applies. The
  sibling-call problem still does: a CPS tail call needs `-O2`'s
  sibling-call optimization
  (docs/reported/cps-self-tail-call-relies-on-sibling-call.md).
  `-O2 -fno-inline-functions` or a similar narrower setting may be the
  middle ground. Measure it.
