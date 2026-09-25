# `#lang r7rs`: a top-level `define` named like a Turmeric form is that form

**RESOLVED 2026-09-25, archived.** The lowering's clash table now also
covers a global the user DEFINES (in a program or a `define-library` body)
or IMPORTS by name (`only`, `rename`) when it is spelled like a Turmeric
special form, so its definition, uses, export and the importer's `only`
are renamed in step (`gen` -> `gen--user`). Scheme syntax that shares a
spelling (`set!`, `do`, `let`, `if`, ...) is exempt, as is a Turmeric form
written in a Scheme file with nothing defining its name
(`r7rs-elaborates-as-saffron`). A `set!` on such a global is looked up
through the rename. Pinned by `tests/fixtures/r7rs-toplevel-form-names`
(both back ends) and the `form-named-exports` case of
`tests/run-r7rs-import.sh`. A Turmeric importer cannot refer such an export
by its bare name (it is `gen--user` in the module), which it could not call
by that name anyway (TUR-W0042). Original report follows.

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
