# `#lang r7rs`: one library per file, named after the file, and no `(export (rename ...))`

**`(export (rename ...))` resolved 2026-09-26; the other two stay open.** A
library exports a definition under a rename, on both back ends and to a
Turmeric importer too. The "module not found" error for a Scheme import now
says the library's name is its path. What is left is several libraries in one
file and a name independent of the path -- see *Still open* below.

**Held by decision 2026-09-26.** One library per file, named after the file,
stays for now (docs/upcoming/r7rs-lang-plan.md, Section 8, question 7): no
work on the two file-shape restrictions until a port needs them. This report
stays as the record of what lifting them would take.

**Severity:** low-medium. Three restrictions on `define-library` that R7RS
does not impose, all from the one-`defmodule`-per-file shape a library lowers
onto. They bite when porting existing Scheme: a file holding a handful of
small libraries has to be split, and an internal name exported under a public
one has to be renamed at every import instead.

## Repro

A second library in the same file:

```scheme
#lang r7rs
(define-library (two a)
  (export aa) (import (scheme base)) (begin (define (aa) 1)))
(define-library (two b)
  (export bb) (import (scheme base)) (begin (define (bb) 2)))
```

```
$ tur run -I libdir p.tur
libdir/two/a.tur:6:1: error: only one define-library per file
(Turmeric: one defmodule per file)
```

The file's path is also the library's name, so the file above must be
`two/a.tur` to be found at all -- `(import (two a))` in a program resolves
`two/a.tur` against the include path, and a `(two a)` library written in
`lib.tur` is "module 'two/a' not found".

An export rename:

```scheme
#lang r7rs
(define-library (mylib3)
  (export (rename internal-add add))
  (import (scheme base))
  (begin (define (internal-add a b) (+ a b))))
```

```
$ tur run -I libdir p.tur
libdir/mylib3.tur:3:11: error: (export (rename a b)) is not supported yet;
export the name and rename at the import
```

Both on the compiled back end and under `tur --interpret`.

## Root cause

`src/compiler/scheme_lower.c`:

- `define-library` lowers to `defmodule`, and `sl->has_library` is a single
  flag, so the second one in a file is refused at scheme_lower.c:3344. Turmeric
  has one module per file, which is what the message says.
- `library_module` (scheme_lower.c:3450) folds the library name's parts into a
  slash-joined module path (`(two a)` -> `two/a`), and module resolution then
  looks for `two/a.tur` on the include path. That is why the name and the
  path cannot disagree.
- The export loop refuses a `(rename a b)` export spec at scheme_lower.c:3359.
  A `defmodule`'s `:exports` list is bare names, so there is no place in the
  lowered form for the public spelling; the import side has `:refer` plus a
  read-time rename, which is why `(rename ...)` works there.

## `(export (rename internal public))`: resolved 2026-09-26

`src/compiler/scheme_lower.c`, the define-library arm, before any declaration
is lowered:

- When the library defines `internal` (a `define`, `define-values` or
  `define-record-type` in its body) and exports it only this way, the
  definition itself is spelled `public` in the module, through the clash
  table (the per-file global respelling `r7rs-toplevel-define-named-like-a-
  turmeric-form` introduced). Every use in the library follows, and so does
  `is_mut`, so a `set!` global keeps its cell, and the procedure keeps its
  static signature for a Turmeric importer.
- Otherwise -- an imported name, or one also exported under another name --
  `public` is defined at the end of the body: a forwarding `defn` for a
  fixed-arity procedure the library defines, else an `any` alias.
- A library global that is itself named `public` is respelled out of the way
  (`public--libN`), and a public name spelled like a Turmeric form goes
  through the clash rename, like a plain export's.
- An identifier exported twice (plainly and as a rename's public name, say) is
  an error naming it, per R7RS 5.6.1.

A definition that only a macro use expands to is not seen by the scan, so its
rename takes the alias path. Pinned by `run-r7rs-import.sh`'s `export-rename`
and `turmeric-imports-export-rename` and `tests/fixtures/errors/r7rs-export-twice`.

## Still open

The two file-shape restrictions. The "module not found" error for a Scheme
import (src/compiler/elab_module.c) now ends with a note in the library's own
spelling -- "a library is found by its name: (two a) must be the file
two/a.tur (or two/a.scm) on the paths above, holding that one define-library"
-- so the restriction is stated where it bites. The directions below for the
other two are unchanged.

## Fix directions

- ~~**`(export (rename internal public))`**~~ -- done, above.
- **Several libraries in one file** needs either several `defmodule`s per file
  in Turmeric -- a language change, not a Scheme one -- or a split in the
  lowering: emit each `define-library` as its own synthetic module under a
  generated path and register the mapping so imports resolve. The second is
  self-contained but makes the emitted-file layout no longer one-to-one with
  the source, which is worth a decision before coding.
- **Name/path independence** follows from whichever of those lands. The error
  states it now (above).

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**A file holds one library, named after the file.**" It covers the
two restrictions still open (the export-rename sentence was trimmed
2026-09-26). When one of them resolves, trim that clause out of the bullet;
when the last one does, delete the bullet whole. The guide's
"Libraries and Turmeric" section states no file-shape rule, so nothing else
needs amending.
