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

**Measured 2026-09-25, second look: most of the `cc` time was one warning.**
`gcc -ftime-report` on `r7rs-named-let-sum`'s 1.2 MB of C put 71% of the
5.2 s in "phase parsing", and `-fsyntax-only` alone took 2.85 s with `-Wall`
and 0.13 s without. Bisecting `-Wall`: `-Wno-misleading-indentation` takes
the syntax check to 0.11 s; no other `-Wno-` moves it. GCC's
misleading-indentation check is quadratic on the long brace-less `if`
chains the Scheme lowering emits. The driver now appends
`-Wno-misleading-indentation` after the user's flags on every `cc` it runs
over emitted C (`TUR_EMITTED_C_CC_FLAGS`, src/main.c), so a harness's own
`TUR_CC_FLAGS` with `-Wall` gets it too:

| program | before | after |
|---|---|---|
| `r7rs-named-let-sum`, `tur build` end to end | 6.4 s | 3.1 s |

What is left: about 0.9 s of `tur emit-c`, 1.4 s of gcc's optimize-and-
generate at `-O2` (1.2 s at `-O0`: the size, not the level), and the link.
The directions below are what the rest would take.

**Measured 2026-09-26, third look: dead-definition elimination would not
help `cc`.** GCC already drops what the program does not reach before it
optimizes anything: `-fdump-ipa-cgraph` on `r7rs-named-let-sum`'s C lists
about 1,100 of the ~1,780 emitted functions under "Removing unused symbols"
in the first pass, ahead of analysis. Its optimize-and-generate time (2.7 s of
2.9 s at `-O2` on a 4-core container) is spent on the ~680 that remain, which
a one-line program genuinely reaches -- `write`, the uncaught-error printer
and the numeric tower pull in most of the prelude. Emitting only reachable
definitions would save the parse of the rest (about 0.15 s) and some of `tur
emit-c`'s 1.0 s, not the `cc` time. The first direction below -- a prelude
compiled once -- is the one that moves the number.

## Fix directions

- Precompile the prelude once: build it as a library (`libr7rs.a`, or an
  object in the tur install) and link it, emitting only declarations into the
  program -- the way the S2 split runtime treats the preamble.
- Or cache the prelude's object by content hash, as the ABI cache does for
  modules.
- Or emit only what the program reaches (dead-definition elimination before
  emission). Measured above: it saves emit time and parsing, not the `cc`
  optimization time, because GCC already drops the unreachable functions and
  a small program reaches ~40% of the prelude.
