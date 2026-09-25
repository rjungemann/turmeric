# `#lang r7rs`: re-entrant `call/cc` is escape-only on Windows

**Severity:** medium. On Linux and macOS a continuation can be invoked after
its `call/cc` has returned (r7rs-lang-plan T5), so generators and coroutines
work. On Windows (MinGW/UCRT) the same program stops at the first re-entry
with the escape-level error, because `call/cc` there is the one-shot escape.
Found by PR 923's Windows CI; five fixtures are skipped there with
`requires.posix-apis` (r7rs-continuations, r7rs-continuation-after-return,
r7rs-gc-basic, region-escape-via-callcc, docs-r7rs-guide-examples).

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
