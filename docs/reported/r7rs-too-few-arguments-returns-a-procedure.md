# `#lang r7rs`: a call with too few arguments returns a procedure instead of raising

**Severity:** medium. R7RS makes calling a procedure with fewer arguments
than it requires an error; here the call is a Turmeric partial application
and returns a procedure, so the program goes on with a wrong value:

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f a . rest) a)
(write (guard (e (#t 'raised)) (f)))   ; writes #<procedure>, not raised
```

Filed 2026-09-27 while landing r7rs-srfi-plan S7: SRFI 41's suite tests
`(test-error (stream-zip))` and `(stream-for-each proc)` with no stream, and
both "pass" through without an error on both back ends. A call to a known
procedure with too many or wrong-typed arguments is refused at compile time
instead ([r7rs-dead-mistyped-call-refused-at-compile-time](r7rs-dead-mistyped-call-refused-at-compile-time.md)),
which is how `(test-error (stream-map odd?))` fails to build compiled.

## Root cause

A Scheme `define` lowers to a Turmeric `defn`, and Turmeric curries a call
that supplies fewer than the required positional parameters (CLAUDE.md,
"Not auto-curried": a variadic defn can still be under-saturated up to its
required parameters, returning a closure). The Scheme lowering does not opt
its procedures out.

## Fix directions

- In a `#lang r7rs` file, lower an under-saturated call to a known procedure
  to a raise of an error object ("f: expects at least 1 argument, got 0"),
  and have the dynamic call path check the count against the procedure's
  arity before it builds a partial application.
- A fixture: `guard` catches the too-few call on both back ends, and
  SRFI 41's two `test-error` cases start passing (raise its floor).
