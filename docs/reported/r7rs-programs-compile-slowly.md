# `#lang r7rs`: every program takes 6-8 s to build

**Severity:** low-medium. A one-line Scheme program takes as long to build as
the whole prelude: 6.4 s for `r7rs-named-let-sum`, 7.6 s for `r7rs-strings`,
on an idle 4-core Linux box with the Debug (ASan) `tur`. CI runners are
slower, and under the suite's parallel load every `#lang r7rs` fixture
overran the 10 s per-fixture budget (PR 923, the `Split runtime (cc path)`
job: 28 `tur build timed out (>10s)`). The fixtures now carry
`expected.timeout` 60; that is the stopgap, not the fix.

## Repro

```sh
time ./build/tur build tests/fixtures/r7rs-named-let-sum/input.tur -o /tmp/x
```

## Root cause

The prelude (stdlib/r7rs/prelude.tur, about 700 top-level definitions over the
numeric tower, strings, ports, the reader and the printer) is elaborated
and emitted into every program, and the C compiler then optimizes all of
it: `r7rs-strings` emits 28,888 lines of C. `tur emit-c` takes about 1.2 s
of the build; `cc -O2` over that one translation unit takes the rest.
`TUR_RUNTIME=split` does not help: a Scheme program's preamble never matches
the committed split artifact (measured the same with and without it).

## Fix directions

- Precompile the prelude once: build it as a library (`libr7rs.a`, or an
  object in the tur install) and link it, emitting only declarations into the
  program -- the way the S2 split runtime treats the preamble.
- Or cache the prelude's object by content hash, as the ABI cache does for
  modules.
- Or emit only what the program reaches (dead-definition elimination before
  emission); a small program uses a fraction of the prelude.
