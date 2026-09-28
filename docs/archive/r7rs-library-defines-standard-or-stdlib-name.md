# `#lang r7rs`: a `define-library` cannot define a standard or stdlib name

**RESOLVED 2026-09-28.** A `define-library` may define and export a standard
name (`square`, `abs`) or an auto-loaded stdlib one (`None`, `list-length`),
plainly or as an `(export (rename in pub))` public name. It works on both back
ends, under every import set. Pinned by `tests/run-r7rs-import.sh`'s
`library-defines-standard-and-stdlib-names` and
`library-standard-names-under-import-sets`. The rest of this file is the
original report.

## Fix

One context-free rule, so the library and each importer agree without seeing
each other's imports: `lib_needs_respelling` (src/compiler/scheme_lower.c)
holds for a standard procedure's name (the rename and on-demand tables) and
for an auto-loaded stdlib global (`std_file_of`, which the module pass has
through `elab_scheme_stdlib_file`). It ignores what the library itself
imports or excludes.

- **Library side.** `note_stdlib_clashes` respells every name the library
  defines that the rule holds for to `<name>--user` (`add_clash`), so the
  definition, every use in the library and the plain export follow. An
  export rename whose public name the rule holds for is spelled
  `<pub>--user` too.
- **Importer side.** `lib_syntax_of`, which already reads the library's
  source for its macros, records which exports the library respells
  (`lib_respelled_exports`). A plain import, and an `only`, `except` or
  `rename` set, binds each kept name to that spelling (`lib_bind_respelled`,
  and the `rename`/`only` arms of `emit_import_spec`). A `prefix` rule
  applies it after taking the prefix off.

A name the program takes from the library means the library's definition,
even when `(scheme base)` is imported beside it without an `except`: the
importer's rename wins. R7RS 5.2 calls two bindings of one name an error,
and the report proposed refusing it. This follows the guide's existing
policy instead, where a program may shadow a standard name as chibi allows.
A Turmeric module importing the library sees `square--user`.

---

**Severity:** medium. A library whose body defines a name that `(scheme base)`
or the auto-loaded Turmeric stdlib also has -- SRFI 1's `list-copy`-style
extensions, a portable library's own `square`, anything called `None` or
`list-length` -- does not compile, or compiles and cannot be reached. Porting
existing Scheme libraries hits it; SRFI bodies do not (they are spliced and
spelled `srfi<N>--<name>`, r7rs-srfi-plan S1), and neither do programs (fixed
2026-09-26, below).

Filed 2026-09-26 while measuring r7rs-srfi-plan S0.

## What is fixed, and what is not

A **program's** top-level definition is the program's own whatever the name
(src/compiler/scheme_lower.c `note_stdlib_clashes`, `type_named_global`):

- a standard name (`square`) shadows R7RS's for the program; the prelude and
  every SRFI keep theirs;
- an auto-loaded stdlib name (`None`, `Vec`, `list-length`) is respelled
  `<name>--user`, found from the stdlib forms in the stream or, for an
  interpreter/REPL session that loaded the stdlib in an earlier call, from the
  environment (`elab_scheme_global_kind`, src/compiler/elab_module.c);
- a Turmeric builtin type name (`any`, `int`, `ptr`) is spelled `<name>--user`
  wherever user code names it -- programs, libraries and importers alike, so a
  library may define and export one (`run-r7rs-import.sh`
  `type-named-exports`).

Pinned by `tests/fixtures/r7rs-program-shadows-names` and
`r7rs-repl-shadowed-name-persists`.

A **library** still is not covered for the first two. `note_stdlib_clashes`
skips a file holding a `define-library` ("a library's names live in its
module"), and nothing respells them.

## Repro

`lb/xsquare.tur`:

```scheme
#lang r7rs
(define-library (lb xsquare)
  (export square)
  (import (scheme base))
  (begin (define (square x) (list 'mine x))))
```

`prog.tur`:

```scheme
#lang r7rs
(import (except (scheme base) square) (scheme write) (lb xsquare))
(write (square 1))
(newline)
```

| Library defines | `tur run` | `tur --interpret` |
| --- | --- | --- |
| `square` | `defn: 'r7rs-square' is already defined by an auto-loaded stdlib module` | `unknown function or operator 'square'` |
| `None` | `defn: 'None' is already defined by an auto-loaded stdlib module` | `defn: 'None' is already defined` |
| `list-length` | `defn: 'list-length' is already defined by an auto-loaded stdlib module` | `(mine 1)` -- and the stdlib's `list-length` is now the library's for everyone |

Expected: `(mine 1)` on both back ends, with the stdlib's `list-length` left
alone.

## Root cause

Two halves, and both have to move together:

1. **The library.** Its body is lowered with the standard renames still on
   (`rn_global`: `square` -> `r7rs-square`, src/compiler/scheme_lower.c), so
   `(define (square x) ...)` becomes `(defn r7rs-square ...)` inside the
   module. A stdlib name is lowered as itself. Either way the definition
   collides with the auto-loaded global.
2. **The importer.** It resolves a bare name with its own tables, not the
   library's: an imported `square` -- even with `(except (scheme base) square)`
   -- resolves as the importer's own name (`excluded`), and nothing says the
   library spelled it differently. Respelling only the library (step 1) turns
   the error into "not exported".

The type-name case needs neither half to know the other because the rule is
context-free: R7RS has no name `int`, so user code's `int` is always a Scheme
definition. `square` and `None` are not context-free -- the first means R7RS's
unless something shadows it, and whether a name is a stdlib name depends on
the stdlib the program joins.

## Fix directions

- **Library side:** respell a defined standard or stdlib name to
  `<name>--user` in the library pass too, and export it under that spelling.
  The stdlib test needs `elab_scheme_global_kind` from the module pass
  (`elab_module.c` passes NULL today, because the library pass also has to
  keep an earlier REPL turn's respellings out -- see the callback's comment
  in scheme_lower.h).
- **Importer side:** read the library's export list and each export's
  spelling. The importer already reads the library's source through
  `SchemeLibResolveFn` for its macros (`lib_syntax_of`); extend that scan to
  record which exports the library respells, and bind each public name the
  import set keeps (after `only`/`except`/`prefix`/`rename`) to
  `<module>/<name>--user`.
- **Conflict check:** importing `square` from both `(scheme base)` and a
  library is two bindings for one name (R7RS 5.2). Once the importer knows a
  library's exports, it can refuse that with the `except` fix in the message,
  as `srfi_bind` does for SRFIs.
