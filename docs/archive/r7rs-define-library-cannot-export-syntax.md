# `#lang r7rs`: a `define-library` cannot export a `syntax-rules` macro

**RESOLVED 2026-09-26.** A `define-library` exports its `syntax-rules`
macros, on both back ends (src/compiler/scheme_lower.c, the `lib_*` block
after `library_defined_names`). The work followed the first fix direction
below: carry the macro's source. It is not in the module interface, because a
macro is expanded by the Scheme lowering before any module is loaded. Instead,
both sides read the library's source through one scan (`lib_scan`), so they
agree on every name:

- **The library** leaves an exported macro out of its module's exports. It
  also exports each of its own definitions that a macro form names (a helper
  the template calls) under a hidden spelling, `<lib>--syntax--<name>`, via
  the export-rename lowering (`lib_rewrite_exports`).
- **An importer** reads the library's file through the module loader's own
  search (`elab_scheme_library_path`, now a parameter of
  `scheme_lower_program`). It renames every macro and every helper its
  templates name to the hidden spellings, registers the macros, and binds each
  exported one under the name the import set gives it: bare, `only`, `prefix`
  or `rename`. It also imports the hidden helpers by name
  (`lib_syntax_of`, `lib_syntax_import`).

So a template's free identifiers keep their library meaning in the importer
(R7RS 4.3.2). A helper the library does not export stays out of the
importer's namespace under its own name, and an importer's own definition of
that name does not capture it. Quoted data in a template is left alone.

Pinned by `tests/run-r7rs-import.sh`, on both back ends:

- `library-exports-macros`: a private macro, an unexported helper, a quoted
  helper name, an importer defining the same helper, and a `.scm` library
  using the first library's macro and exporting a macro under
  `(export (rename ...))`;
- `library-macros-under-import-sets`: `only`, `prefix` and `rename`;
- `turmeric-imports-macro-library`: a Turmeric importer still gets the
  procedures.

**What it does not do:**

- A Turmeric module that imports the library gets its procedures, not its
  macros; there is no `syntax-rules` in Turmeric.
- A template that names something the library *imports* from a third library
  (rather than defines) is not renamed. It resolves in the importer today,
  because a loaded module's names are visible to the compile unit, but
  without the hidden-spelling guarantee: an importer defining that same name
  would capture it. Carrying those names too means knowing the third
  library's exports at this point; take that up if a real library needs it.
- An importer reads each Scheme library it imports one extra time, once per
  lowering pass, to look for macros. That is small next to a Scheme
  program's build.


**Severity:** medium. R7RS 5.6.1 lets a library export any identifier it
defines, keywords included. In practice exporting a macro is how most
libraries ship their syntax. Here, exporting a `define-syntax` name is a
compile error, so a Scheme library cannot give its importers new syntax. Found
while planning SRFI support (docs/upcoming/r7rs-srfi-plan.md). That plan works
around the gap by splicing SRFI libraries into the importer rather than
compiling them as modules, so SRFI work does not wait on this fix, but user
libraries still hit it.

## Repro

Measured 2026-09-26 at fdd51fc9, Debug build; the same on both back ends.

`mymac.tur`:

```scheme
#lang r7rs
(define-library (mymac)
  (export my-rec twice)
  (import (scheme base))
  (begin
    (define (twice x) (* 2 x))
    (define-syntax my-rec
      (syntax-rules ()
        ((_ (name . args) body ...) (letrec ((name (lambda args body ...))) name))))))
```

`main.tur`, next to it:

```scheme
#lang r7rs
(import (scheme base) (scheme write) (mymac))
(write ((my-rec (f n) (if (= n 0) 1 (* n (f (- n 1))))) 5))
```

```
$ tur run main.tur
./mymac.tur:2:1: error: exported symbol 'my-rec' is not defined in this module
./mymac.tur:2:1: note: ensure 'my-rec' has a matching (defn ...) or (defmacro ...) inside the (defmodule mymac ...) body, ...
```

For comparison, the same macro in a file brought in with `(load "util.scm")`
works on both back ends, including referential transparency (a template's
helper keeps its definition-site meaning under a use-site rebinding). The
expander is fine; what is missing is the way to carry a macro across a module
boundary.

## Root cause

`define-library` lowers to a `defmodule` (r7rs-lang-plan D9). A
`define-syntax` in the library body is consumed by the Scheme lowering's own
`syntax-rules` expander (`sr_define`, src/compiler/scheme_lower.c) and emits
nothing, so the export list names a symbol with no `defn` or `defmacro`
behind it. The export check in src/compiler/elab_module.c:1580 then refuses it.
The importing file is lowered in a separate pass, whose macro table
(`SL.macros`) never sees the library's macros.

## Fix directions

- Carry the macro's source. When a library exports a `define-syntax` name,
  record the `syntax-rules` form (and the library's respelling of the free
  identifiers its templates use) in the module's interface. An importing
  Scheme file's lowering then registers it like a local `define-syntax`.
  Referential transparency needs the template's free identifiers to resolve to
  the library's exported (or respelled private) names, and the
  `(load ...)` path shows the expander already handles that when the names
  are global.
- Or lower the macro to a Turmeric `defmacro` the module system already
  exports. That is harder, because Turmeric macros are unhygienic and the
  Scheme expander's renaming would have to survive the round trip.
- The export check must stop counting a `define-syntax` name as undefined.
- A fixture in `tests/run-r7rs-import.sh`: a library exporting a macro and a
  procedure its template uses but does not export. Run it on both back ends.
