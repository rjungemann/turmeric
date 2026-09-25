# `#lang r7rs`: `include` and `include-ci` are refused

**Severity:** low-medium. Both back ends. R7RS 4.1.7 `include` splices the
forms of a file into the program, and 5.6.1 allows `(include ...)` as a
`define-library` declaration -- the standard way to keep a library's body in
its own file. Both are refused with a named error, at top level and inside
a library. Documented in docs/guides/r7rs-guide.md ("Where it differs") and
r7rs-lang-plan 9.3; pinned as an error fixture,
`tests/fixtures/errors/r7rs-include-deferred`.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(include "helper.scm")            ; helper.scm: (define (helper x) (+ x 1))
(write (helper 1))                ; want 2
```

```
$ tur run inc.tur              # and tur --interpret
inc.tur:3:1: error: include is not supported yet: an included file would
have to be read as Scheme without its own #lang line; put the definitions
in a define-library and import it
```

Inside a library:

```scheme
(define-library (helpers) (export helper) (include "helper.scm"))
; error: (include "file") in a library is not supported yet; write the
; definitions in a (begin ...)
```

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug).

## Root cause

Two named refusals in the Scheme lowering: top-level and expression
`include` / `include-ci` at src/compiler/scheme_lower.c:2997-3001, the
library declaration at 3185-3186. The reason the message gives is real: the
`#lang` line selects the reader per FILE, and the lowering has no way to
read a second file under the Scheme reader (`scheme_enabled`) without one.
T4's `load` (stdlib/r7rs/eval.tur) reads a file at run time through the
embedded evaluator, which is a different thing: it does not splice into the
compilation unit, and it costs the interpreter link.

## Fix directions

- Give the lowering a "read this path as Scheme" entry point: the reader
  with `scheme_enabled` forced on and the `#lang` check skipped, resolving
  the path against the including file's directory, as R7RS asks. The
  forms come back as `Form`s and are spliced in place of the `include`
  form; the same call answers the library declaration.
- `include-ci` is the same read with the case-folding flag on
  (`#!fold-case`, which the reader already has).
- Diagnostics: spans in an included file should name that file. The
  diagnostic file registry already keys spans to files; register the
  included path.
- A fixture with an included file and one with an included library body,
  on both back ends; then delete `errors/r7rs-include-deferred`.
