# `#lang r7rs`: one library per file, named after the file, and no `(export (rename ...))`

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

## Fix directions

- **`(export (rename internal public))`** is the cheapest of the three and the
  one a port hits most: emit the module's export under the internal name, and
  record the public spelling in the same table the import side's rename uses,
  so an importer of the library asks for `public` and resolves `internal`. The
  machinery exists (`lower_import_set`'s rename path); this is the same map in
  the other direction, applied at the definition site.
- **Several libraries in one file** needs either several `defmodule`s per file
  in Turmeric -- a language change, not a Scheme one -- or a split in the
  lowering: emit each `define-library` as its own synthetic module under a
  generated path and register the mapping so imports resolve. The second is
  self-contained but makes the emitted-file layout no longer one-to-one with
  the source, which is worth a decision before coding.
- **Name/path independence** follows from whichever of those lands; until then
  it is worth stating in the error, which currently reports only "module not
  found" with a search list and never says the library's name is its path.

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**A file holds one library, named after the file.**" It covers all
three restrictions. When one of them resolves, trim that clause out of the
bullet; when the last one does, delete the bullet whole. The guide's
"Libraries and Turmeric" section states no file-shape rule, so nothing else
needs amending.
