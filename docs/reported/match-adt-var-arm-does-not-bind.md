# A variable catch-all arm on an ADT match does not bind its variable

**Severity: low-medium** -- `(match o (OA n) n whole (g whole))` is
`TUR-E0003: unbound symbol 'whole'`, so the only usable catch-all on an ADT
match is `_`. Found 2026-08-21 while implementing
[match-nested-constructor-patterns](../archive/match-nested-constructor-patterns.md),
whose fallback for a nesting group is exactly this shape.

## Repro

```turmeric
(defdata Outer (OA :int) (OB))

(defn f [o : Outer] : int
  (match o
    (OA n) n
    whole  (match whole (OA i) i (OB) -1)))
;; error [TUR-E0003]: unbound symbol 'whole'
```

The same arm shape on a PRIMITIVE scrutinee works -- `(match n 1 "one" x x)`
binds `x` -- so this is specific to the ADT path.

## Root cause

Two halves, both needed:

- `src/compiler/elab_structs.c`, the ADT arm loop's `pat_form->tag == F_SYM`
  branch: a non-`_` symbol sets `pat->is_var` / `pat->var_sym` and then says
  "No new scope needed; elaborate body directly". It never calls
  `binding_new`, never opens an arm scope, and never sets `pat->var_binding`.
  The literal-match path 500 lines above does all three.
- `src/compiler/emit_expr.c`: the ADT arm emitters test
  `pat->is_wildcard || pat->is_var` together and emit no binding for either.
  The literal-match emitter has the `pat->is_var && pat->var_binding` arm that
  declares the C variable from the scrutinee temp.

## Fix direction

Mirror the literal path in both places: create the binding with the
scrutinee's type, scope it around the arm body (and its when-guard, as the
literal path was already corrected to do), and in the emitter declare
`<ctype> <name> = <scrutinee>;` at the top of the arm. Two sites in the
emitter (the tag-switch form and the if-chain form).

Worth checking while there: whether the bound variable should be NARROWED the
way `scrut_narrow_binding` narrows a bare-symbol scrutinee -- a var arm after
some constructor arms is not narrowable to one variant, so plain scrutinee
type is right.

## Guides to update when fixed

- docs/guides/sum-types-guide.md (the pattern-matching bullet list mentions
  `_` only)
