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
