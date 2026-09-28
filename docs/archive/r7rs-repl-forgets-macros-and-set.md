# `#lang r7rs`: a REPL turn forgets what the turns before it set up

**RESOLVED 2026-09-28**, except `prefix` over a user library (below). Found
and fixed the same day, while fixing
[r7rs-repl-echoes-multiple-values-opaquely](r7rs-repl-echoes-multiple-values-opaquely.md).
Pinned by the hook fixture `tests/fixtures/r7rs-repl-macros-and-set-persist`
and by `tests/fixtures/r7rs-eval-macros-and-set-persist`.

**Severity:** medium. Ordinary REPL moves failed at `tur repl --lang r7rs`,
and the first two also failed through `eval` in `(interaction-environment)`,
on both back ends:

```sh
printf '%s\n' \
  '(define-syntax sw (syntax-rules () ((_ a b) (list b a))))' '(sw 1 2)' \
  '(define n 0)' '(set! n 5)' \
  '(import (srfi 1) (srfi 26))' '(fold + 0 (map (cut * 2 <>) (list 1 2)))' \
  | tur repl --lang r7rs
```

```
error: unknown function or operator 'sw'
error: set!: 'n' is immutable; use ^mut at the binding site to allow it
error: unknown function or operator 'fold'
```

A turn that imported a library of the user's did not run its own
expressions, and its definitions were private to a module nobody could
reach:

```
> (import (lb mac)) (display "ran") (define (zz) 1)
=> #<procedure>
> (zz)
error: symbol 'zz' is private to module '-eval-'
```

chibi and Racket do all of these.

## Root cause

The interpreter elaborates a session incrementally (TR2.2b,
`turi_eval_impl` in src/turi/eval.c). Each turn hands the elaborator only
its own forms, and earlier definitions resolve out of the session's scope.
The Scheme lowering (`scheme_lower_program`, src/compiler/scheme_lower.c)
runs on those forms alone, so what one turn set up in the lowering was gone
in the next:

- **Macros.** A `define-syntax` registers a macro in the lowering's table for
  the call. The next turn's table started empty.
- **Mutability.** A top-level variable is lowered as `^mut name : any` only
  when a `set!` of it appears in the same unit (`collect_muts`). A `set!` in
  a later turn is not in the unit that lowered the `define`, so the variable
  was an immutable `def`.
- **Imports.** An SRFI's names (`fold` -> `srfi1--fold`), its macros, a
  `rename` or `except`, and a library's exported macros are rename tables and
  macro entries in the lowering, set up by the `import` form. A later turn
  had none of them. A library's *procedures* did carry over, because they
  live in the session's module scope.

The whole-program fallback (the first turn, or a turn after a failure) had
every turn in its stream and did not show these.

Separately, a turn that imports a library needs a Turmeric `import`, which
is legal only inside a module. The lowering wraps such a program in a
`defmodule` with its expressions in a synthesized `main`. At the prompt
nothing calls `main`, and the wrapper module did not export the turn's
definitions.

## Fix

- The interpreter tells the lowering which raw forms the session's earlier
  turns hold. It does this around each incremental elaboration and each
  replayed turn (`scheme_lower_set_session_prior`).
- A REPL turn first runs `session_macros_load`. That covers a Scheme form
  from a synthetic `<...>` source, which includes `eval`.
  - It replays the earlier turns' `import` sets for their lowering state
    (`session_imports_load`). What those imports emit is dropped, because
    the earlier turn already did it.
  - It registers the `define-syntax` forms those turns left standing. A later
    top-level `define` of the same name drops the macro, whether it was in an
    earlier turn or this one. A later `define-syntax` replaces it.
- In a REPL turn, a top-level variable `define` is always the mutable `any`
  cell, since a later turn may `set!` it (`SL.repl_turn`). A procedure
  definition stays a `defn`: a later turn redefines it with `define`, which
  already worked.
- A REPL turn that imports a library gets a module holding only the imports
  (`r7rs-repl-imports`). The turn's own definitions and expressions stay at
  the session's top level, where the imported names are visible.

## Not covered

`(import (prefix (lb mac) m:))` of a user library at the prompt: `m:helper`
reads as `m/helper`, and the alias `m` is scoped to the imports module, so
it is unknown even in the same turn. Before this fix that turn's
expressions did not run at all. `prefix` over an SRFI or a `(scheme ...)`
library works, because that is a rename in the lowering. Import the library
plainly, or with `only` or `rename`.
