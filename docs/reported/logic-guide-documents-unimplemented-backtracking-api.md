# logic-programming-guide.md presents an API summary that does not exist

**Severity: medium** (docs) -- the guide's "API Summary" (`choice-point`,
`run`, `run*`, `do-backtrack`, `constraint`, `return`, `(instance Clone ...)`
syntax) misleads users into calling nonexistent forms. Found in the 2026-08-20
docs audit.

## Repro

`grep -rn "choice-point\|do-backtrack" stdlib/ src/` -> no hits;
`(run 1 [x] ...)` from the guide does not elaborate.

## Root cause

The guide predates `stdlib/logic.tur`; its examples are a design sketch over
the real primitives. `cloneable-reset`/`cloneable-shift` exist
(src/compiler/elab_call.c:2328); the miniKanren engine ships as
`stdlib/logic.tur` with `mzero`/`mreturn`/`mplus`/`mbind`/`fresh`/`run-logic`.

## Fix direction

Either implement a thin `choice-point`/`run`/`do-backtrack` macro layer over
`stdlib/logic.tur` + cloneable continuations, or rewrite the guide's examples
onto the `tur/logic` names (and `definstance` instead of `instance`).

## Guides to update when fixed

- docs/guides/logic-programming-guide.md (primary)
- docs/guides/tur-logic-guide.md (cross-link when resolved)
