# `#lang r7rs`: every program takes 6-8 s to build

**RESOLVED 2026-09-28**, by the first fix direction: the prelude is compiled
once and linked. `tur build` writes a `#lang r7rs` program as two C units, a
library unit (the runtime preamble and the auto-loaded stdlib) whose object
is cached, and the program's own unit, which declares what it uses from the
library. Measured 2026-09-28 on a 4-core container, gcc 13:

| `tur build`, end to end | one unit | split, library cached |
|---|---|---|
| `r7rs-named-let-sum`, Release `tur` | 3.0 s | 0.95 s |
| `r7rs-strings`, Release `tur` | 4.65 s | 1.55 s |
| `(display 1)`, Release `tur` | 2.4 s | 0.87 s |
| `r7rs-named-let-sum`, Debug (ASan) `tur` | 4.0 s | 2.35 s |

**The first build costs more than it used to: 8.5 s** for
`r7rs-named-let-sum` on an empty cache, where one unit took 3.0 s. The
library unit exports every stdlib definition, so cc cannot drop the ~1,100
functions a given program never reaches, as it does in one unit. It pays
that once per library, per `tur` version, `cc` and flags; every later
build is the cached column. Builds that start together on a cold cache
(the fixture suite, `make -j`) wait on a lock for the first one's object
rather than each compiling their own. Four concurrent cold builds did one
library compile and finished in 9.4-10 s. Making that first compile cheaper
is filed as docs/reported/r7rs-prelude-library-cold-compile.md.

Of the 95 `#lang r7rs` fixtures, all 95 build split. 79 link the same
library object and the other 16 use 6 variants (see below), so the cache is
warm after the first few builds. Pinned by
`tests/check-r7rs-prelude-split.sh` (ctest `tur_r7rs_prelude_split`). The
rest of this file is the original report.

## Fix

**Phase A: let cc drop what nothing reaches.** Two things the emitter wrote
kept dead code alive in every program. The fat-box table was filled by a
startup function that referenced every procedure used as a value anywhere,
dead code included. It is now a static initializer made of address
constants (`TUR_FATBOX_DEF`), so cc drops a box nothing references, along
with the function it points to. The musttail pin
(r7rs-raise-musttail-fails-under-clang-x86-64) was one `used` table naming
every function that makes a `TUR_MUSTTAIL` call, which kept all of them
alive. Each such function now pins itself (`TUR_MUSTTAIL_SELF`, an asm
reference inside its own body), so the pin is dropped when the function is.

**Phase B: two units.** The emitter runs twice over the same elaborated
program (`emit_split_set_mode`, src/compiler/emit_split.h):

- **The library unit** holds the runtime preamble and every stdlib
  definition the driver auto-loaded, with external linkage, and nothing of
  the program. It does not depend on the program, so its object is cached
  under `<tmpdir>/tur-build/prelude/<hash>.o`. The hash is an xxh64 of
  `TUR_VERSION`, the `cc`, the flags and the unit's text. The object is
  about 740 KB.
- **The program unit** declares the library's functions and state and
  defines only the program. Stateless helpers (the preamble's `static`
  functions, ADT constructors) are written into both units, and each unit
  compiles the ones it calls.

Runtime state lives in the library unit only. Text the emitter writes
verbatim (the preamble, hoisted inline C, a stdlib file's C block) goes
through `emit_split_state`. In the library unit that function removes
`static` from file-scope variables. In the program unit it turns them into
`extern` declarations, turns function definitions into prototypes, and
demotes constructors to unused functions. Every name the split makes
external is prefixed `tur_sl_` in both units. Without the prefix, the
preamble's state (`g_tur_any_types`, `tur_scheduler`, ...) would interpose
on libturi.a's S2 runtime, which an `eval` program links. The library
emission hands the program emission three tables:

- the key and name of each fat box, so a procedure's identity stays one box;
- each stdlib function's C signature, where any mismatch refuses the split;
- the exported names.

The static-init runs as `__tur_split_lib_init`, which the program calls
first.

The split is refused, and the program built as one unit exactly as before,
whenever the emitter meets something it cannot divide:

- a stdlib top-level statement or module `defer`;
- a thread-local global;
- a stdlib `defdynamic`;
- a signature mismatch.

If either `cc` or the link fails, the driver also builds one unit
(`cmd_build` retries). `TUR_PRELUDE_SPLIT=0` forces one unit.
`TUR_SHOW_CC=1` prints why a split was declined and keeps the library's `.c`
beside its object. The split covers `tur build` and `tur run` of a
`#lang r7rs` program on 64-bit Linux. Elsewhere it is off unless
`TUR_PRELUDE_SPLIT=1`. On Windows the shared keyword records resolve
wrongly (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md). On
macOS a value kept only in a Turmeric map is collected
(docs/reported/r7rs-prelude-split-gc-seam-on-macos.md). Both showed up in
the first CI run.
It does not cover `--debug` (line
directives), project builds, or Turmeric programs. `tur emit-c` still writes
one unit, so no `expected.c` snapshot changed shape.

**Making the library text program-independent.** The report's objection to a
hash cache was that the prelude's C differs between programs. Two changes
make it stable:

- **Binding ids.** Ids minted while the stdlib loads now come from their own
  counter, starting at `ELAB_STDLIB_ID_BASE` (1,000,000). A program's
  declarations therefore no longer shift the stdlib's `_3428` suffixes. This
  renames stdlib-local ids in the 155 snapshots that carry stdlib bodies.
- **Where the rest of the variance comes from.** A program that hands a
  colored closure to a stdlib higher-order function makes that function CPS,
  so its library differs. So does a program that imports more of the stdlib
  (`(scheme file)`, `(scheme eval)`). Each such library is its own cache
  entry.

**Latent bugs the split exposed (fixed here):**

- **CPS fresh binders shared an id space with binding ids.** The capture
  analysis keys both on `id`. A source binder whose id equalled a fresh
  one's was taken as bound where it was free: a handler's `k_7` was missing
  from the environment of a reopen frame whose slot was `__t7`. This stayed
  hidden while every program binding id sat above the stdlib's thousands.
  Fresh ids now start at `CPS_FRESH_ID_BASE` (0x80000000).
- **CPS temporaries and the direct emitter's temporaries share a spelling.**
  Both are `__t<n>`: the CPS IR counts per function, and `fresh_tmp` counts
  across the whole program. A one-unit build had emitted the stdlib first,
  so the emitter's count was in the hundreds by the time a user function was
  emitted. The program unit emits no stdlib, so a delegated node's `__t0`
  redeclared the term's own `__t0`. Rendering a CPS term now raises the
  emitter's count past the term's (`SEnt.fresh_n`).
- **ASan's fake stack hides roots from the conservative collector.** The
  prelude's `__asan_default_options` turns off use-after-return, and it
  lives in the library object. A program linked with ASan (anything that
  links the ASan `libturi`, e.g. `eval`) therefore has its library object
  compiled with `-fsanitize=address,undefined` too. Otherwise the default
  options are lost, and a GC-torture `eval` corrupts its heap.

**Not changed:** the r7rs fixtures keep `expected.timeout` 60, for the
cold-cache first wave of a suite run.

## Original report

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
