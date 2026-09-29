# `#lang r7rs`: the first build compiles the whole stdlib (8.5 s)

**RESOLVED 2026-09-29, by the parallel-pieces direction.** A cold cache
compiles the library unit in one piece per CPU (up to eight), at `-O2`, and
links the pieces into the one cached object: 9.1 s to 4.6 s on four cores
with a Release `tur`, 6.3 s on two, and no run-time cost that measures. A
single-CPU machine still pays the whole compile, once. See *Resolution*.

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
that a CPS tail call used to need (cps-self-tail-call-relies-on-sibling-call;
since 2026-09-28 self and mutual CPS tail calls are jumps at any level, and
only the closure and `guard` shapes that report lists still lean on it). It
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
  sibling-call problem mostly does not either: a CPS self or mutual tail call
  is a jump since 2026-09-28, and only a tail call through a closure or after
  a `guard` still needs the sibling call
  (docs/archive/cps-self-tail-call-relies-on-sibling-call.md).
  `-O2 -fno-inline-functions` or a similar narrower setting may be the
  middle ground. Measure it.

## Resolution (2026-09-29): the library unit in pieces

`emit_split_pieces` (src/compiler/emit_split.c) cuts the library unit into
`n` texts; `prelude_compile_pieces` (src/main.c) compiles them at once from
one shell (`cc ... & p0=$!; ...; wait $p0 || ok=1; ...`) and joins them with
`cc -r -nostdlib` into the object the cache already keeps, so nothing about
the link changes. Any failure -- a piece that does not compile, the partial
link -- falls back to the whole compile: slower, never wrong. `n` is the
number of online CPUs up to eight; `TUR_PRELUDE_JOBS=<n>` overrides it and
1 compiles whole; Windows (no `&` in cmd.exe, and the split is off there)
compiles whole.

The parse is 0.15 s of the 8.2 s; the rest is `-O2` over ~1,200 external
functions. So every piece keeps the whole unit's declarations, types and
static helpers, and defines a share of the functions:

- **External functions** go to pieces in source order, in runs of about
  equal size. The emitter writes a stdlib file's definitions together and
  they mostly call each other, so a run keeps callers beside callees. Sorted
  by size and dealt out instead, the `guard`/`parameterize` path lost 5%:
  calls inlined in one unit crossed pieces.
- **Small functions are copied.** An external function whose body is at most
  1,200 bytes, keeps no local `static`, does not name itself or its frame
  (`__func__`, setjmp, alloca, asm) and carries no symbol attribute gets a
  `static` twin in every other piece: a `static` prototype after its first
  prototype, then `#define f(...) tur_sd_f(__VA_ARGS__)`. The macro rewrites
  direct calls only, so `-O2` inlines them as before, while `f` without a
  call -- an address, a comparison of procedures -- still names the one
  external definition. The definition below the macro becomes the twin (a
  declaration with no storage class after a `static` one keeps internal
  linkage).
- **State is defined once.** A file-scope variable is defined in piece 0
  and declared `extern` in the others. A `static` one loses `static` and
  moves to the prefix `tur_sp_`, so nothing a runtime archive defines can
  bind it. Read-only data is copied. A constructor runs from one piece.

`tests/check-r7rs-prelude-split.sh` builds the library from a cold cache
whole and in four pieces, and fails when a writable data name is defined
twice in the pieces' object and more times than whole -- the hazard this
report named. With the static variables left alone in pieces 1-3 it
reports `r7k_form_depth (4 vs 1)` and the other three.

Measured on a 4-core Linux box, gcc 13.3:

| `r7rs-named-let-sum`, Release `tur` | cold cache | warm |
| --- | --- | --- |
| whole (`TUR_PRELUDE_JOBS=1`) | 9.0-9.2 s | 1.0 s |
| 2 pieces | 6.2-6.4 s | 1.0 s |
| 4 pieces (the default here) | 4.5-4.7 s | 1.0 s |

Clang 18, Debug `tur`: 7.8 s to 4.8 s. Run time, user-time medians of 12
alternating runs, against the whole unit's object: the prelude benchmark
above 1.472 s whole, 1.421 s in pieces; a `guard`/`parameterize`/
`dynamic-wind` loop 1.868 s and 1.873 s. The box's noise is a few percent,
and the differences sit inside it. (With no copies of small functions both
were about 7% slower; with a copy limit of 2,500 or 4,000 bytes no faster
than 1,200, and the compile grows.)

Two directions tried on the way and dropped:

- **gcc's LTO partitions** (`-flto -c`, then `-flto=auto -r` with
  `-flinker-output=nolto-rel`): 1.4 s serial plus 3.3 s parallel, one TU so
  no state can fork -- but 4-7% slower at run time, gcc only, and not a
  thing ld64 does.
- **Every other piece's function as `extern inline __attribute__((gnu_inline))`**,
  to keep all of the unit's inlining: gcc then treats every one as declared
  inline, and a piece compiled for over ten minutes.

What is left is the one-CPU case, which is the whole compile, and the cache's
home in `<tmpdir>`: the "warm the cache at install" direction, if a reboot or
a fresh CI runner still costs too much.
