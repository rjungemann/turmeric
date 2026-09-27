# `#lang r7rs`: a primitive applied to the wrong type panics; `guard` cannot catch it

**Severity:** medium. `(car 5)` reached through a variable aborts the program
with a Turmeric panic. It does not raise an R7RS error that `guard` or
`with-exception-handler` can catch. chibi and Racket raise a catchable error
here, and portable code (test suites especially, SRFI 64's `test-error`
among them) relies on it.

Filed 2026-09-27 while landing r7rs-srfi-plan S2.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (h x) (car x))
(write (guard (e (#t (list 'caught (error-object? e)))) (h 5)))
(newline)
```

Expected `(caught #t)`. Both back ends instead end the program:

```
panic: cast: any holds int, not R7rsPair
```

(compiled: `panic at /tmp/tur-build/..._tur.c:NNNN: cast: any holds int, not
R7rsPair`, then `Aborted`, exit 134).

## Root cause

The prelude's procedures take typed parameters (`(defn r7rs-car [p :
R7rsPair] ...)`, stdlib/r7rs/prelude.tur:116). A Scheme value arrives as
`any`, and the dynamic-file call inserts a checked cast. On a tag mismatch
that cast panics (the Saffron boundary check, r7rs-lang-plan's "checked
cast at the boundary"), and a panic is not an R7RS condition: the prelude's
`guard` and `with-exception-handler` see only `raise`d objects.

## Fix directions

- In a `#lang r7rs` file, lower the boundary cast's failure to a `raise` of
  an error object (`error-object-message` along the lines of "car: not a
  pair", with the value as an irritant) instead of a panic. The Saffron
  boundary may stay a panic; the choice is per dialect.
- Or give the prelude's hot procedures an `any` parameter and a
  `(if (pair? p) ... (error "car: not a pair" p))` guard. That is clearer per
  procedure but has to be repeated across the prelude, and it costs a branch
  where the cast was a tag compare.
- Whichever it is, a fixture: `guard` catches `(car 5)`, `(vector-ref '() 0)`
  and `(+ 'a 1)` on both back ends, and `error-object?` holds for each.
- [r7rs-dead-mistyped-call-refused-at-compile-time](r7rs-dead-mistyped-call-refused-at-compile-time.md)
  is the compile-time half: once a statically mistyped call is let through to
  run time, this is the failure it produces.
