# The JIT fixture suite paid for the whole prelude on every program

**Severity: medium (CI wall-clock; gated).** Resolved 2026-09-29 by
`src/compiler/jit_prune.{h,c}`. Found while driving rjungemann/turmeric#970,
whose `JIT engine (macos-latest)` leg failed with `tur_jit_fixture_tests`
killed at its ctest `TIMEOUT 1500` -- the "next occurrence" that
[macos-jit-leg-stall-unexplained](../reported/macos-jit-leg-stall-unexplained.md)
was waiting for. It was not a stall: the harness was still printing `PASS`
lines until shortly before the kill. The suite had simply become slow enough to
reach the limit on a 3-core runner (`main`'s run of the same hour passed the
leg, in 27 minutes for the whole job).

Raising the timeout was the wrong fix. This records why the suite was slow.

## Where a JIT fixture's time went

Measured on the Debug + `-DTUR_JIT=ON` tree (the CI configuration: `tur` under
ASan/UBSan, MIR itself `-O3` but running on ASan's allocator), 4-core Linux,
`TUR_JIT_TIMING=1`:

- **Each program paid ~0.46 s of fixed cost** before doing anything of its own.
  Fixtures under one second were 75% of the suite's time, so the fixed cost
  *was* the suite.
- **c2mir was ~280 ms of that (60%).** It compiled
  - ~20 system headers the committed decls region includes (about two thirds of
    c2mir's time), and
  - the **entire auto-loaded prelude** -- ~377 static functions, ~160 KB of C
    for `(println 42)` -- that the program never calls (about one third).
- The front end (elaborating the stdlib) was most of the rest, ~150 ms.

The cc path never paid the second item: cc drops an unreferenced `static`
function without compiling it. c2mir compiles every definition it is handed.
So under `tur jit` a one-line program compiled the whole prelude, plus the
socket/inet/regex/hamt headers the prelude needs, every time.

Two further findings from the same measurement:

- **Every `#lang r7rs` and `#lang saffron` program disengages the S2 split**
  (`TUR_JIT_TIMING  split  disengaged  preamble hash != committed artifact`).
  Their preamble is not the committed one, so they compile the full runtime
  preamble as well; an r7rs fixture pushes ~1.5 MB of C through c2mir. They
  are ~200 of the corpus's fixtures and the most expensive ones.
- **Four fixtures fell back to cc only because of prelude code they never
  call**: `dead-base-ctor-trap`, `generic-closure-return-type-app`,
  `once-basic`, `typeclass-assoc-type-method-return` hit c2mir parse/link
  errors in unused prelude functions.

## The fix

`jit_prune_split_source` / `jit_prune_full_source` (called from `cmd_jit` in
`src/main.c`, right after `jit_try_split_preamble`) do in text what cc does
at link time:

1. **Program-half reachability.** The TU is cut into top-level chunks. A
   `static` function definition or prototype, a `static` object, a
   `TUR_FATBOX_DEF`, or an `extern` declaration is a removable node; everything
   else (macros, typedefs, type definitions, non-static definitions, the
   hoisted prefix, the decls region or full preamble) is a root. Identifiers
   are read outside comments and literals, a worklist marks what the roots
   reach, and unreached nodes are dropped.
2. **Heavy includes (split TU only).** `<regex.h>`, `<arpa/inet.h>`,
   `<netinet/in.h>`, `<sys/socket.h>`, `<sys/select.h>` and `"hamt.h"` in the
   decls region are dropped when no identifier they declare survives in the
   program's own text. A full preamble keeps every include -- its own runtime
   uses them. A dropped `"hamt.h"` leaves its own `<stdint.h>`, `<stdbool.h>`
   and `<stdio.h>` behind, so the TU's first system header is unchanged (see
   the macOS note under Results).

`TUR_JIT_NO_PRUNE=1` turns it off. Pruning the decls region's ~500 prototypes
as well was measured (4 ms of c2mir's 67, unsanitized) and not done.

**Safety net.** A reference the scan misses cannot pass silently. The pruned
TU is always a *copy*; if c2mir fails to compile or link it, `cmd_jit` retries
the full TU in the engine (TUR-W0071, now also for a TU the split declined),
and `tests/run-jit.sh` **fails** any fixture that passes only on that retry.
Before this change a W0071 retry that succeeded was an ordinary `PASS`.

## Results

Full `tests/run-jit.sh`, 4-core Linux, sanitized, harness settings
(`ASAN_OPTIONS=detect_leaks=0`):

| | before | after |
| --- | --- | --- |
| `(println 42)` TU handed to c2mir | 242 KB | 103 KB |
| c2mir, `(println 42)` | ~280 ms | ~167 ms |
| c2mir, `(println 42)`, standalone unsanitized | 141 ms | 71 ms |
| sum of per-fixture wall-clock | 1843 s | 1355 s |
| median fixture | 508 ms | 350 ms |
| passed via the cc fallback | 22 | 18 |

The four fixtures above were reclaimed by the engine and removed from
`tests/jit-fallback-baseline.txt`.

r7rs and Saffron fixtures (the split declines them; A/B of
`TUR_JIT_NO_PRUNE=1` against the default on the same tree, `-P4`):

| | TU to c2mir | sum | median |
| --- | --- | --- | --- |
| `r7rs-*` (104) | ~1.47 MB -> ~0.78 MB | 354 s -> 297 s | 2990 -> 2561 ms |
| `saffron-*` (94) | ~445 KB -> ~305 KB | 63.5 s -> 53.9 s | 672 -> 577 ms |

The first cut of the pruner found three real misses on its first corpus run
(`capability-module-roundtrip`, `time-import-module`, `r7rs-threads-tls`):
an inline-C block pasted with its indentation puts a second `static`
definition on an indented line, which the chunker leaves in the chunk above,
and that chunk was named for its first definition only. Such a chunk is now a
root. All three had passed silently before the W0071 check existed.

**The first CI run then failed most of the macOS JIT corpus with W0071 while
Linux passed.** Dropping `"hamt.h"` -- the decls region's first include --
made `<ucontext.h>` the TU's first system header, and the region includes it
under `#define _XOPEN_SOURCE 700`. glibc fixes its feature level with
`_DEFAULT_SOURCE` (defined at the top of the region), so it did not notice.
macOS's `<sys/cdefs.h>` settles `__DARWIN_C_LEVEL` once, on its first
inclusion: under `_XOPEN_SOURCE` without `_DARWIN_C_SOURCE` that is the POSIX
level, which hides the Darwin extensions from the rest of the TU. A program
that still uses a map keeps `hamt.h` first and should have been unaffected.
The dropped `hamt.h` now leaves its three system includes in
its place, so the header order ahead of that block is what it was. The
mechanism is inferred from the header order and the macOS headers' documented
behaviour; the console never showed the c2mir diagnostic. `run-jit.sh` now
prints the first engine error on each W0071 FAIL line and fails its one-program
smoke test on W0071, so a host-wide break is named once, with its reason.

## What is left

- **The system headers the decls region genuinely needs** are now the largest
  single piece of c2mir's time on a split program.
- **ASan's allocator under in-process c2mir.** The same compile is ~2.5x slower
  inside sanitized `tur` than in a standalone unsanitized driver. That is the
  CI configuration by design (`TUR_DEBUG_SANITIZE` defaults ON), so it is a
  multiplier on everything above rather than a separate defect.
- **r7rs and Saffron programs never get the S2 split.** The probe preamble
  `emit_rt_split_source` produces for them does not hash to the committed
  artifact. Pruning now removes their unused prelude, but they still compile
  the full runtime preamble. Making the split cover those dialects is the next
  lever for the suite's slowest fixtures.
- **The front end** elaborates the whole stdlib for every program: ~150 ms
  sanitized for `(println 42)`, but **1.2-1.5 s for an r7rs program**, about
  55% of an r7rs fixture's time now (engine ~0.8-1.1 s). With ~130 r7rs
  fixtures at a ~2.5 s median, that is the largest remaining block of the
  suite.
- A timing trap for anyone measuring by hand: the harness runs with
  `ASAN_OPTIONS=detect_leaks=0`. Without it, LeakSanitizer's exit scan over an
  r7rs program's allocations (the collector is compiled out under the JIT)
  adds ~4 s per program -- `r7rs-strings` is 2.6 s with it off and 6.7 s with
  it on.
