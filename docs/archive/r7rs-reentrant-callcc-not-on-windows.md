# `#lang r7rs`: re-entrant `call/cc` is escape-only on Windows

> **RESOLVED 2026-09-26 (archived).** Both fix directions below, in both
> back ends -- the prelude's inline C (stdlib/r7rs/prelude.tur) and its
> interpreter twin (src/turi/interpreter_natives.c):
>
> - **The base.** `r7k_stack_base` reads the TEB's `NT_TIB.StackBase` at
>   `gs:[0x08]`, the win64 spelling of `NtCurrentTeb()->NtTib.StackBase`.
>   Read directly rather than through `GetCurrentThreadStackLimits`: the
>   prelude's block is hoisted above the preamble's `<windows.h>`, and a
>   hand-written kernel32 declaration ahead of it would clash with its
>   `dllimport`.
> - **The jump.** `R7K_SETJMP`/`R7K_LONGJMP` are `__builtin_setjmp`/
>   `__builtin_longjmp` on Windows (GCC/clang), plain `setjmp`/`longjmp`
>   elsewhere -- the same choice the preamble's `TUR_SETJMP` makes. The
>   builtins restore the frame and stack pointers and unwind nothing, which is
>   what a jump into a stack image just copied back needs. `__builtin_longjmp`
>   may not share a function with its `__builtin_setjmp`, so the per-form
>   runner's jump back into its own frame goes through a `noinline`
>   `r7k_longjmp`. The guard-page worry in the directions below did not
>   materialise: the restore's `alloca` only probes pages the captured stack
>   already committed.
>
> **Not covered: c2mir.** Code compiled by c2mir (`tur jit`) has neither
> `__GNUC__` nor the builtin, so a Windows JIT compile keeps plain `setjmp`
> and finds no stack base, and `call/cc` stays the escape there. No program
> reaches that today: a Scheme program never engages the S2 split
> ([r7rs-programs-compile-slowly](../reported/r7rs-programs-compile-slowly.md)),
> and the whole-preamble JIT path falls back to `cc` on Windows (`__va_start`,
> [jit-windows-support-spike](../reported/jit-windows-support-spike.md)) -- so
> `tur jit` on Windows runs a Scheme program through the fixed `cc` path. A
> `tur_sjlj_set`/`_jump` arm (the symbols the split's DK trampoline uses) was
> written and then taken out rather than shipped unrun.
>
> **Verified with a MinGW-w64 cross build (gcc 13) under Wine 9.0, not a real
> Windows box.** Each measurement against the unchanged prelude first:
>
> | check | before | after |
> | --- | --- | --- |
> | generator (one form, four re-entries), compiled | `0`, then the escape error | `0 10.5 21.0 31.5 done` |
> | the same, `tur.exe --interpret` | `0`, then the escape error | the same five lines |
> | the six skipped fixtures, compiled, `-O2` and `-O0` | -- | 6/6 each |
> | every `#lang r7rs` fixture with an `expected.stdout`, compiled | -- | 42 passed; the 4 others are harness artifacts (3 negative fixtures whose expected stderr the harness folds into stdout) and `r7rs-complex`, whose `(exp 0+3.141592653589793i)` has a different `sin(pi)` residue in its imaginary part (1.2246063e-16 against 1.2246468e-16) under Wine's libm -- identically with the old prelude |
> | the same, `tur.exe --interpret` | -- | 42 passed, 1 failed (`r7rs-complex` again) |
>
> The widest-reaching change is not the re-entry but that EVERY `call/cc` on
> Windows now copies the stack, where before each one took the escape; the
> full-corpus rows are what cover that. On Linux the r7rs subset is unchanged:
> `run.sh` 92/0, `run-turi.sh` 76/0, `run-jit.sh` 89/0.
>
> The six fixtures lose their `requires.posix-apis` markers, so the Windows CI
> leg's full suite is the real-Windows confirmation. The report's repro below
> predates the per-form prompt: on every platform it now prints `2` then `2`
> (the re-entry finishes the `write` form and carries on after the `if`) --
> r7rs-toplevel-reentry pins that; the generator above is the one to use.

**Severity:** medium. On Linux and macOS a continuation can be invoked after
its `call/cc` has returned (r7rs-lang-plan T5), so generators and coroutines
work. On Windows (MinGW/UCRT) the same program stops at the first re-entry
with the escape-level error, because `call/cc` there is the one-shot escape.
Found by PR 923's Windows CI; six fixtures are skipped there with
`requires.posix-apis` (r7rs-continuations, r7rs-continuation-after-return,
r7rs-gc-basic, region-escape-via-callcc, docs-r7rs-guide-examples, and
since the per-form prompt, r7rs-toplevel-reentry). A capture declines
without a stack base whatever the per-form base says (PR 928's Windows CI
found the prompt's base letting one through, which crashed every program
that used call/cc as an escape).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define k #f)
(define n 0)
(write (+ 1 (call/cc (lambda (c) (set! k c) 1))))
(newline)
(set! n (+ n 1))
(if (< n 3) (k n))
```

Linux and macOS print 2, 2, 3. Windows prints 2, then fails.

## Root cause

A T5 continuation copies the C stack from the `call/cc` to the thread's
stack base. `r7k_stack_base` (stdlib/r7rs/prelude.tur, and its interpreter
twin in src/turi/interpreter_natives.c) knows the base only through
`pthread_getattr_np` (glibc) and `pthread_get_stackaddr_np` (macOS). With no
base, `r7rs-cont-capture__` declines and `r7rs-call/cc` uses the escape.

## Fix directions

- The base: `NtCurrentTeb()->NtTib.StackBase`, or
  `GetCurrentThreadStackLimits` (Windows 8+).
- The jump is the hard part. MinGW-w64's `longjmp` on x64 unwinds through
  SEH to the target frame, and after the stack image is copied back the
  frames it would walk are the restored ones, not the ones that called
  `longjmp`. Use `__builtin_setjmp`/`__builtin_longjmp` (no unwinding) for
  the capture and restore on Windows, and check the guard page: the restore
  already grows the stack past the image with alloca, which `__chkstk`
  probes.
- Test on a Windows runner before un-skipping the fixtures.

## Guide upkeep

Two places in `docs/guides/r7rs-guide.md` say this, and both go when it
resolves: the "Where it differs from R7RS" bullet beginning "**Re-entrant
`call/cc` is Linux and macOS only.**" (delete whole), and the sentence in
Control starting "On Windows it is escape-only for now" (delete; the paragraph
above it already states the re-entrant behavior).
