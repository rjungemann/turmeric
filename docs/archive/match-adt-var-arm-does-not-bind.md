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

## Resolution (2026-08-21)

Fixed in both halves the report names, and they were both needed -- the
elaborator change alone gets past the "unbound symbol" and then fails in the C
compiler with `'whole_1336' undeclared`.

- `elab_structs.c`: the ADT arm loop's var branch calls `binding_new` with the
  scrutinee's type and elaborates the arm body inside a scope holding it, the
  same shape the literal path uses.
- `emit_expr.c`: both ADT arm emitters declare the variable from `__scrut`.
  Three reads, because `__scrut` is bound three different ways: `*__scrut` for a
  pass-by-pointer by-value scrutinee, `__scrut` for a by-value aggregate, and
  `(T)(intptr_t)__scrut` for the carrier pointer (the switch-dispatch form is
  always the last).

The report's open question -- whether the bound variable should be narrowed the
way `scrut_narrow_binding` narrows a bare-symbol scrutinee -- is answered as it
guessed: no. A var arm sits after some constructor arms and is reached for any
remaining variant, so there is nothing to narrow it to; it gets the scrutinee's
own type.

Pinned by `tests/fixtures/match-adt-var-arm/`, which covers all three
representations (carrier ADT, by-value `:copy` product, switch form) plus a var
arm reached from inside a nested-pattern group's fallthrough -- the shape that
found this defect in the first place. Passes under `run.sh` and `run-turi.sh`
alike.
