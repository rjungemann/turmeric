# `#lang r7rs`: a re-entrant `call/cc` gives a wrong value when the unit also calls `eval`

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

## Fix directions

1. Reconcile with CI first (above). A host- or toolchain-specific failure and
   a semantic one want different work.
2. `tur emit-c` both variants -- with and without the `eval` call -- and diff
   what happens to `count-to`. The difference is a compile-time one by
   construction, so it is in that diff.
3. v0.56.2 changed exactly this area (`bb27a64d3` escape-only `call/cc`
   copies nothing and nested CPS entries drop their reap list, `27fb767cf`
   interpreter images as deltas, `906a6a44d` the reap-list drop kept to
   single-threaded programs). Whether this predates them is untested --
   0.56.1 and 0.56.0 are the bisect points.
