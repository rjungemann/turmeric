# `#lang r7rs`: a procedure body cannot name a top-level variable defined after it

**Severity:** medium. Both back ends. R7RS 5.3.1 lets a procedure body refer
to any top-level variable, wherever its `define` stands, as long as the
variable is defined by the time the body runs. Turmeric resolves a
non-procedure global at its use site, so the reference is "unbound".

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f) (* y 2))
(define y 21)
(display (f)) (newline)
```

```
$ tur run p.tur           # and tur --interpret
p.tur:3:16: error: unbound symbol 'y'
```

chibi and Racket print `42`. Mutual recursion between procedures works
(`(define (even? n) ... (odd? ...))` before `(define (odd? n) ...)`): a
`define` of a lambda is a `defn`, and the elaborator pre-declares every
`defn` (elab_toplevel.c, "Pass 1"). A plain variable is not pre-declared.

Found 2026-09-25 writing `tests/fixtures/r7rs-toplevel-order`.

## Fix directions

- Pre-declare every top-level `define`d variable of a Scheme program as an
  `any` global before elaborating the bodies, the way `defn`s are, and let
  its `def` fill it in. The Scheme lowering already knows the full set (it
  scans the program for `set!` targets); the pre-declaration is one more
  walk over the same forms.
- A read before the initializer runs would then see the placeholder value
  rather than an error; chibi signals "unbound variable" at run time in that
  case. A checked read is the R7RS-faithful version.
