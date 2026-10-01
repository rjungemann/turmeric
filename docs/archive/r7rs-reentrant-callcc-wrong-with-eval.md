# `#lang r7rs`: a re-entrant `call/cc` gives a wrong value when the unit also calls `eval`

**RESOLVED 2026-10-01 by `7c90e00b8` ("r7rs: keep thread-locals out of the
call/cc capture's setjmp function"), which landed ~10 hours after the commit
this was filed against and was never in it.** The last hypothesis below -- that
this was the Mac's OS-ahead ASan runtime rather than a code bug -- is **wrong**,
and the measurement that settles it is the one the report asked for, run on the
same host class it was filed from (macOS 27.0 / build 26A5378n, Apple clang
21.0.0 / CLT 27.0.0, arm64, `cmake -DCMAKE_BUILD_TYPE=Debug`, sanitizers ON):

| Build | `repro.scm` (with `eval`) | without `eval` |
| --- | --- | --- |
| `bf31e725c` (the commit filed against) | **`error: +: not a number #<unknown>`** | `3` |
| `7c90e00b8^` (`f5ce6e336`) | **SEGV in `r7rs_hycont_hycapture_un_un+0x670`** | -- |
| `7c90e00b8` | `3` then `42` | -- |
| `8bb60d016` (main) | `3` then `42` | `3` |

So it is a code bug, fixed; the toolchain is not implicated. Two things rule
the ASan theory out directly rather than by inference:

- The repro still fails at `bf31e725c` **today**, under the current CLT. If the
  cause were an outdated ASan runtime, updating the toolchain would have fixed
  the old commit too. It did not.
- At `main` the failing configuration is exactly the one that now works. The
  emitted program links the ASan-instrumented `libturi.a` (2472 undefined
  `asan` symbols) and `otool -L` on the binary shows
  `@rpath/libclang_rt.asan_osx_dynamic.dylib` loaded -- and it prints `3` and
  `42`. "Sanitized `libturi.a` in a program that copies and restores its own
  stack" is not the trigger.

**The cause**, from `7c90e00b8`: the re-entry path's thread-local stores --
the DK trampoline's pair, `g_dk_driver`, `__dk_entry_depth` -- were made
through a **stale address** after `setjmp`'s second return. The capture now
saves and restores that state through two prelude functions
(`r7rs-cont-save-state` / `r7rs-cont-load-state`) called through volatile
function pointers, so the function that calls `setjmp` touches no
thread-local and every access outside it recomputes its address.

That also explains both of this report's puzzles, which it had treated as one:

- **Why `eval` mattered.** A store through a stale address does damage that
  depends on where the address lands, and `(scheme eval)` links the embedded
  interpreter -- changing the program's module count and static-TLS layout, and
  so where that address points. Same bug, two presentations: it corrupted the
  delivered value at `bf31e725c` (`+: not a number`) and landed in text at
  `f5ce6e336` (SEGV at `str x8, [x9]`). The report read "a later form changes
  an earlier result" as proof of a *compile-time* difference; it is a link-time
  difference, which is what the 2026-09-29 Linux narrowing had already found
  when it diffed the emitted `count-to` and saw no change.
- **Why CI was green.** `7c90e00b8`'s own message says macOS CI (arm64, Apple
  clang) *did* die on this fixture's `count-to` -- it was being diagnosed on
  `macos-latest` in parallel with this report. The two were the same bug seen
  from two sides, which is why "CI is green and this is real" never reconciled:
  the green run predated the re-entry regression reaching it.

Guide upkeep: `tests/fixtures/docs-r7rs-guide-examples` passes at `main` on this
host, all 12 expected lines, so the guide's `call/cc` example is covered again.

---

**Severity:** medium. On the compiled back end, a `call/cc` whose
continuation is stored and re-entered reads back a non-number for a variable
`set!` between the capture and the re-entry, and the program dies with
`error: +: not a number #<unknown>`. The interpreter gives the right answer.
The trigger is somewhere else in the file entirely: an `(eval ...)` call. The
same program without it is correct.

Found 2026-09-28 while landing r7rs-srfi-plan S2's SRFI 17, as the
`docs-r7rs-guide-examples` fixture failing locally. **It is not caused by
that change** -- it reproduces with the SRFI 17 commit's compiler diff
reverted.

**Not seen in CI.** `Test (macos-latest)` and `Test (ubuntu-latest)` are both
green at bf31e725c (run 36455992864), which is the commit this reproduces on.
The local host is macOS 27.0 / Apple clang 21.0.0 / arm64, `cmake
-DCMAKE_BUILD_TYPE=Debug`, and the failure survives deleting the `tur-build`
temp directory. Why CI does not see it is not explained here, and finding
that out is the first step: until then, treat "CI is green" and "this is
real" as both true and reconcile them before chasing codegen.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme eval))
(define (count-to n)
  (let ((k #f) (i 0))
    (call/cc (lambda (c) (set! k c)))
    (set! i (+ i 1))
    (if (< i n) (k #f) i)))
(write (count-to 3))
(newline)
(write (eval '(* 6 7) (environment '(scheme base))))
(newline)
```

```
$ ./build/tur run repro.scm
error: +: not a number #<unknown>          # expected: 3, then 42

$ ./build/tur --interpret repro.scm
3
42
```

`count-to` is the `call/cc` example from `docs/guides/r7rs-guide.md`, so the
guide's own fixture carries it: `tests/fixtures/docs-r7rs-guide-examples`
stops after `[in]body[out]` and loses its last eight lines.

## What narrows it

- **The `eval` CALL is the trigger, not the import.** Delete the last two
  lines and `(write (count-to 3))` prints `3`, with `(scheme eval)` still
  imported. Put them back and the earlier `count-to` fails. A later form
  changing an earlier result means a compile-time difference -- CPS
  colouring, or the shape `eval`'s presence forces on the unit -- not
  anything about evaluation order.
- **Compiled only.** `--interpret` is right on every variant.
- **Alone it is fine.** `count-to` in a file with no `eval` is correct on
  both back ends, so the escape-vs-re-entrant `call/cc` decision
  (`callcc_escape_only`, src/compiler/scheme_lower.c) is right in isolation.
  This continuation is stored in `k` and re-entered, so it is the re-entrant
  path.
- The value that comes back wrong is `i`, a `let` variable `set!` between
  the capture and the re-entry -- so the suspicion is the captured stack
  image, or the boxing of a mutable variable a captured continuation spans.
  **That is a lead, not a finding**: nothing here has been read out of the
  emitted C.

## Narrowed on Linux (2026-09-29)

Linux x86-64, a Debug `tur` at `72245ef5` (main) and at the branch that made
the dynamic environment per thread:

- **It does not reproduce.** The repro prints `3` and `42` built with gcc 13
  (`-O2`, one unit and the prelude split alike), with clang 18 (through a
  `-DTUR_DEBUG_SANITIZE=OFF` build, since this box has no clang ASan runtime
  to link a sanitized `libturi.a` against), and under `tur jit`.
  `tests/fixtures/docs-r7rs-guide-examples` passes.
- **The emitted `count-to` is the same with and without the `eval`.**
  `tur emit-c` of both variants, temporaries renumbered: `count_hyto__cps`
  and its continuation `count_hyto_j0` are identical. What the `eval` adds
  is two top-level forms, the program unit's own keyword records for the
  quoted `*`, `scheme` and `base` with a `__tur_symtab_seed()` call in
  startup, two fat boxes -- and, at link time, the embedded interpreter
  (`libturi.a`, which a Debug `tur` builds with ASan, so the program then
  carries the ASan runtime too). So step 2 below is done, and the
  "compile-time difference" is not in `count-to`'s code: it is in the link
  or in startup.
- **Not the prelude split's macOS seam.** At `bf31e725c` macOS built one
  unit (`prelude_split_applies` declined off Linux), so the constructor-order
  bug fixed in `a73ab97c`
  ([r7rs-prelude-split-gc-seam-on-macos](r7rs-prelude-split-gc-seam-on-macos.md))
  cannot be it.
- **ASan in the process is not enough on its own.** On Linux the gcc variant
  runs with the ASan runtime loaded (LeakSanitizer reports the embedded
  interpreter's 3-byte leak at exit) and is right.

What is left points at the Mac's toolchain pairing: macOS 27 with Apple
clang 21 is the OS-ahead-of-toolchain case
[macos-asan-runtime-deadlocks-at-startup](../reported/macos-asan-runtime-deadlocks-at-startup.md)
describes, and the one variant that fails is the one that loads that ASan
runtime (through `libturi.a`) into a program that copies and restores its own
stack. The cheapest next measurement is on that Mac: the repro with a Release
`tur` (no sanitizer in `libturi.a`), or with a `-DTUR_DEBUG_SANITIZE=OFF`
Debug one. Right there and wrong under a sanitized `libturi.a` would make
this a toolchain report, not a codegen one.

## Fix directions

1. Reconcile with CI first (above). A host- or toolchain-specific failure and
   a semantic one want different work.
2. ~~`tur emit-c` both variants -- with and without the `eval` call -- and diff
   what happens to `count-to`.~~ Done 2026-09-29: identical (above). The
   difference is in the link (the embedded interpreter and, from a Debug
   `tur`, its ASan runtime) or in startup, not in `count-to`.
3. v0.56.2 changed exactly this area (`bb27a64d3` escape-only `call/cc`
   copies nothing and nested CPS entries drop their reap list, `27fb767cf`
   interpreter images as deltas, `906a6a44d` the reap-list drop kept to
   single-threaded programs). Whether this predates them is untested --
   0.56.1 and 0.56.0 are the bisect points.
