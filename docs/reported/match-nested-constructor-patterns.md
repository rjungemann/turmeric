# match arms cannot nest constructor patterns

**Severity: medium** (expressiveness) -- `(match e (Add (Lit 0) r) r ...)`
fails to elaborate; every fixture and guide flattens with an inner `match`.
Found in the 2026-08-20 docs audit.

## Repro

```turmeric
(match e
  (Add (Lit 0) r) r
  ...)
;; fails to elaborate; the workaround is
(match e
  (Add l r) (match l (Lit 0) r ...))
;; see tests/fixtures/adt-nested/input.tur, which uses exactly this shape
```

## Root cause

The match elaboration in src/compiler/elab_structs.c binds one constructor
level per arm; sub-patterns in field positions are treated as binders only.

## Fix direction

Recursive pattern compilation (decision-tree or nested-if lowering) with
exhaustiveness extended to nested depth.

## Guides to update when fixed

- docs/guides/gadts-guide.md ("No nested patterns in GADT arms" limitation)
- docs/guides/gadts-cookbook.md
- any other pattern-matching material that teaches the flattening workaround
