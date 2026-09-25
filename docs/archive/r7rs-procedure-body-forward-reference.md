# `#lang r7rs`: a procedure body cannot name a top-level variable defined after it

**RESOLVED 2026-09-25.** The Scheme lowering marks a variable define that
an earlier form names as a `set!` target (`note_forward_defs`,
scheme_lower.c), so its def is `(def ^mut y : any init)`; and the
elaborator's Pass 1 now pre-declares every top-level def of that exact shape
ahead of the bodies (`elab_pre_declare_any_mut_def`, elab_toplevel.c; the
defmodule and imported-module pre-passes call it too), the way it
pre-declares every `defn`, with `elab_def` filling the pre-declared binding
in when Pass 2 reaches the def. The initializer still runs where the define
stands. Works on both back ends, for a program with or without imports and
for a library body; `tests/fixtures/r7rs-forward-reference` covers a
quoted datum, a literal, a string, a `case-lambda` value, a variable that
holds a lambda the program later `set!`s, a define inside `begin` and an
initializer that reads an earlier forward-referenced variable. A read
before the define runs is an error in R7RS (chibi: "unbound variable") and
is unspecified here: the compiled program reads the unset `any` word, the
interpreter reports the variable unbound. A `(def ^mut x : any ...)` in a
Turmeric or Saffron file gets the same pre-declaration, so a defn above it
may name it. Original report follows.


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
