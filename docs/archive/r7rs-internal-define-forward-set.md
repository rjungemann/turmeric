# `#lang r7rs`: `set!` on a later internal `define` is "not bound"

**RESOLVED 2026-09-25, archived.** A body's value define that an earlier
definition's init mentions (`(define (a) (set! b 1)) (define b 0)`, a
closure reading a later variable) is hoisted: a mutable cell bound around
the whole body, assigned in place, so every name the body defines is in
scope throughout it (letrec*). The Scheme lowering's assignment conversion
then boxes it as it does every assigned-and-captured variable, so the
closure sees the assignment on both back ends. Pinned by
`tests/fixtures/r7rs-internal-define-forward-set`. Original report
follows.

**Severity:** low-medium. Legal R7RS is refused on both back ends. Internal
definitions are `letrec*` (R7RS 5.3.2): every name a body defines is in scope
throughout the body, so a procedure defined early may `set!` a variable
defined after it.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f)
  (define (a) (set! b 1))
  (define b 0)
  (a)
  b)
(write (f))          ; want 1
```

```
$ tur run f.tur        # and tur --interpret
f.tur:4:21: error: set!: 'b' is not bound
```

A REFERENCE to `b` from `a`'s body is not the problem. The `set!` is refused
because the body's definitions are bound one after another, and `b` does not
exist yet where `a`'s lambda is lowered.

Found writing r7rs-lang-plan T5's generator example, which worked around it
by defining the variable first (`(define resume #f)`, then `set!` it).

## Root cause (lead)

`lower_body` / `lower_body_inner` (src/compiler/scheme_lower.c) turns the
body's `define`s into a sequence of bindings. The `set!` check (the lowering's
mutable-variable pass, `ac_walk` / `rebind_muts`) runs against the scope as it
stands at each definition, not against all of the body's names.

## Fix directions

Bind every internal-definition name of a body up front (as `letrec*` does):
declare them all (as `#<unspecified>`-initialized mutable cells), then assign
them in order.
