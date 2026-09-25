# `#lang r7rs`: a top-level `define` named like a Turmeric form is that form

**Severity:** low-medium. A Scheme program that names a global `gen`,
`handle`, `perform`, `resume`, `return`, ... gets a Turmeric special-form
error at the use site, on both back ends.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define gen (lambda () 42))
(write (gen))
```

```
$ tur run g.tur        # and tur --interpret
g.tur:4:8: error: gen requires a capture vector and at least one body form: (gen [] body)
```

## Root cause (lead)

R10 renamed every identifier the user's code BINDS locally -- formals, the
`let` family, `do`, `guard` variables (`user_binders`, src/compiler/scheme_lower.c)
-- so a local named like a Turmeric special form is not elaborated as that
form. A top-level `define`'s name is not among them, so `(gen)` still heads a
call with the special form's name.

Found writing r7rs-lang-plan T5's generator example (the global was renamed).

## Fix directions

Give global definition names the same treatment: when a top-level `define`
(or a `define-library` export) names a Turmeric special form, rename it
through the lowering's clash table as a local binder is renamed.
