# `#lang r7rs`: `define-record-type` is refused inside a body

**Severity:** low-medium. R7RS 5.5 makes `define-record-type` a definition, so
it may appear wherever internal definitions may -- at the start of a `lambda`,
`let`, `letrec` or `when` body. Here it is accepted only at the top level or
in a `define-library` body; anywhere else is a compile-time error on both back
ends.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f)
  (define-record-type <box> (mk v) box? (v box-v))
  (box-v (mk 7)))
(write (f))
```

```
$ tur run p.tur
p.tur:4:3: error: define-record-type is only allowed at the top level or in a
library body
$ tur --interpret p.tur        # the same
```

A record type defined at the top level and used inside the procedure works,
so the workaround is to hoist it -- at the cost of the name's scope.

## Root cause

`src/compiler/scheme_lower.c`, the `define-record-type` arm: the form lowers
to a Turmeric `defstruct` plus the constructor, predicate, accessor and
modifier defns, all of which are top-level declarations. A body is lowered as
an expression sequence, which has nowhere to put them, so the arm rejects the
form rather than emitting something that does not type.

## Fix directions

- Lift the generated declarations out of the body to the enclosing top level,
  renaming the struct and its procedures to a fresh gensym per occurrence, and
  bind the names the body used as ordinary locals pointing at the lifted ones.
  The lowering already has the gensym machinery `syntax-rules` hygiene uses.
- A body's `define-record-type` cannot escape its scope in a conforming
  program (its accessors are the only way to reach the fields), so nothing
  needs to be re-entrant: one lifted struct per source occurrence is enough
  even when the enclosing procedure is called many times.

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**`define-record-type` is a top-level or library-body form.**"
Delete it whole when this resolves.
