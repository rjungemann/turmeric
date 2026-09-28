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

**Measured 2026-09-26, r7rs-srfi-plan S0: that holds for the prelude, not
for a spliced Scheme library.** chibi's SRFI 1 spliced into a one-line
program adds about 1.6 s to its build (3.46 s -> 5.03 s), called or not, and
gcc keeps 190 of the 318 functions it adds. `__tur_fatbox_init` fills a
static closure at startup for every procedure the program uses as a value
anywhere, dead code included. A variable define (`(define reverse! reverse)`)
is also initialized at startup. Both reference functions gcc would otherwise
drop. The plan's decision is a whole-program pass that drops unreferenced
`stdlib/srfi/` definitions before emission (its S0 note and S3).

**Measured 2026-09-27, fourth look (a 4-core container, gcc 13).** The
numbers are larger than the 2026-09-25 box's, and the split is the same.

| `r7rs-named-let-sum` | Debug (ASan) `tur` | Release `tur` |
|---|---|---|
| `tur build`, end to end | 4.4 s | 3.55 s |
| `tur emit-c` | 1.36 s | 0.29 s |

The rest is `cc` over 29,400 lines. On the same C:

| `cc` on the emitted C | time |
|---|---|
| `-O2 -Wall` (what `tur build` runs) | 3.3 s |
| `-O2` | 3.2 s |
| `-O1` | 1.95 s |
| `-O0` | 1.76 s |
| `-fsyntax-only -Wall` | 0.18 s |
| link | 0.04 s |

- **No pass dominates.** `-ftime-report` at `-O2` puts 2.84 s of the 3.38 s
  "opt and generate" phase in "callgraph functions expansion", spread over the
  ~680 functions the program reaches, with no single pass over 0.2 s. A flag
  will not fix this.
- **`-O1` is not an option.** It saves 40%, but a CPS function's tail call to
  another is a plain C tail call that needs `-O2`'s sibling-call optimization
  (docs/reported/cps-self-tail-call-relies-on-sibling-call.md). Compiling
  only the prelude at a lower level would stop gcc inlining `car`/`cdr` into
  the program across the attribute mismatch.
- **Parallel LTO helps wall time only.** `cc -O2 -flto=4
  -flto-partition=balanced` takes 2.3 s wall where one process takes 3.6 s,
  on four idle cores. It spends more CPU in total, so it does nothing for the
  suite, which already fills every core. It is also spelled differently on
  clang and needs the linker plugin. Not taken.
- **A content-hash object cache of "the prelude part" would miss.** Emitting
  two fixtures (`r7rs-named-let-sum`, `r7rs-strings`) gives C that differs in
  16,740 lines. An on-demand library (`stdlib/r7rs/read.tur`) splices in more
  types and functions, and binding-id suffixes on globals
  (`r7rs_hyhandlers_un_un_3428`) shift with what was loaded first. The
  prelude's C is not a fixed text today, so caching it by hash of each
  program's emission would rarely hit.
- **This session's changes did not move it.** The one-line program's C is
  348 lines longer after r7rs-type-errors-are-uncatchable-panics and the
  identity fix. Its `-O2` compile time is the same within noise (3.0-3.2 s
  both).

So the first direction below is the one that moves the number, and it has
to compile the prelude independently of the program:

1. Make the prelude, and each on-demand library, a separately compiled unit
   with a header. It needs a stable C interface: exported function names with
   no binding-id suffix, and its globals and runtime state extern rather than
   `static`. The machinery exists for modules: `emit_implementation` and
   `emit_header`, the per-TU `any` registries that merge at startup
   (any-type-ids-are-per-tu), and the ABI cache.
2. Build it once per `tur` install (or per stdlib content hash, in the build
   dir's cache) and link it. The program's TU then carries declarations
   plus the program.
3. The interpreter keeps loading the prelude source. The change is on the
   compiled back end only.

The costly parts are the two dynamic pieces a Scheme program's C carries per
unit: the fat-box and static-init registrations, and the pasted call/cc and
collector runtime. Both need the extern/registry treatment the `any` tables
already have.

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
