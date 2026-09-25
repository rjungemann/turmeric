# `#lang r7rs`: `(except ...)` in an import set is refused

**RESOLVED 2026-09-25, archived.** The import lowering folds an import set
of any nesting (`only`, `except`, `prefix`, `rename`, in any order) into one
spec, unwinding each name to the library's spelling through the modifiers
inside it, and emits it once. Over a `(scheme ...)` library, whose names
are global, an excluded name stops meaning the library's -- so
`(except (scheme base) assoc)` lets the program define its own `assoc` --
and `only` keeps a name the library's. Over a user library or a Turmeric
module, `only` is a `:refer` list, `prefix` an `:as` alias, `rename` a
refer plus a read-time rename, and `except` a full import (Turmeric's
import has no "all but"; an excluded name of such a module is simply not
hidden). Pinned by `tests/fixtures/r7rs-import-sets` (both back ends) and
the `nested-import-sets` case of `tests/run-r7rs-import.sh`;
`errors/r7rs-import-except` is gone. Original report follows.

**Severity:** low-medium. Both back ends. R7RS 5.2 gives four import-set
modifiers -- `only`, `except`, `prefix`, `rename` -- and lets them nest.
`#lang r7rs` accepts three, refuses `except` with a named error, and refuses
any nesting. A program that shadows one standard name (the common reason to
write `except`) has to spell out every other name it uses with `(only ...)`.
Documented in docs/guides/r7rs-guide.md ("Where it differs") and
r7rs-lang-plan 9.3; pinned as an error fixture,
`tests/fixtures/errors/r7rs-import-except`.

## Repro

```scheme
#lang r7rs
(import (except (scheme base) car) (scheme write))
(write (cdr '(1 2)))
```

```
$ tur run except.tur           # and tur --interpret
except.tur:2:9: error: (except ...) is not supported: Turmeric's import has
no "all but"; list the names with (only ...) instead
```

Nesting, which R7RS allows, is refused the same way:

```scheme
(import (prefix (only (scheme base) car cdr) b:))
; error: nested import sets are not supported yet; use one of
; only/prefix/rename directly on the library name
```

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug).

## Root cause

The import lowering (src/compiler/scheme_lower.c:3291-3304) maps a Scheme
import set onto Turmeric's `(import mod :refer [...])` / `:as`, and
Turmeric's import has no "all but these" form. `only` becomes `:refer`,
`prefix` an alias, `rename` a read-time table; `except` has nothing to
become, so it is refused at 3303. The nesting refusal at 3299 is the same
lowering handling one modifier per set.

## Fix directions

The Scheme side knows every library's export list, so `except` (and
nesting) can be resolved before the Turmeric import is written:

- For a standard library, the resident names are the prelude's
  `r7rs-*` table (`SCHEME_LIBS[]`, scheme_lower.c:319, and the name map
  above it); for an on-demand library, the file's definitions; for a user
  `define-library`, its `(export ...)` declaration, which the lowering
  already reads.
- Compute the set: start from the export list, apply each modifier inside
  out (`except` removes, `only` keeps, `prefix`/`rename` re-spell), and
  emit one `:refer` list plus the existing rename table.
- Then the nesting refusal goes too, since a nested set is just the same
  fold applied twice.

Preserve: Turmeric's `import` keeps its own grammar; the fold lives in the
Scheme lowering.
