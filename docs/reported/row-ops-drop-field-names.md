---
status: open
severity: medium
discovered: 2026-07-28
area: compiler (type-level rows, src/compiler/types.c)
---

# Row operations discard a labeled row's field names

## Summary

`row-canon`, `row-union`, `row-concat`, and `row-intersect` all rebuild their
result with `type_typerow()`, which leaves `field_names = NULL`. A labeled row
(`#row{id : int}`) that passes through any of them comes out as a *positional*
row, so the name-distinctness guarantee that
`tests/fixtures/errors/typed-field-row-name-mismatch` pins down silently
evaporates: two rows with the same element types but different labels start
unifying.

Found while writing `docs/guides/row-types-guide.md`.

## Repro

    $ cat > /tmp/r.tur <<'EOF'
    (defstruct Tbl [^&cols] (rows :int))
    (defn make-id [n : int] : (Tbl (row-canon #row{id : int}))
      (:: (make-struct Tbl n) (Tbl (row-canon #row{id : int}))))
    (defn use-name [t : (Tbl (row-canon #row{name : int}))] : int (.rows t))
    (defn main [] : int (use-name (make-id 1)))
    EOF
    $ ./build/tur check /tmp/r.tur; echo "exit=$?"
    exit=0

Expected: `TUR-E0001`, a row mismatch between `#row{id : int}` and
`#row{name : int}` -- exactly what the same program reports without the
`row-canon` wrapper.

`row-union` reproduces it with a no-op union against the empty row
(`(row-union #row{id : int} #row{})` vs `(row-union #row{name : int} #row{})`),
and `row-concat` / `row-intersect` likewise.

## Root cause

`src/compiler/types.c:3732` -- `type_typerow_canonical()` sorts a copy of
`elements` and returns `type_typerow(a, elems, n)`. `field_names` is neither
permuted alongside the sort nor passed on, and `type_typerow()` sets it to
`NULL` (`types.c:3606`).

The same omission is in `type_typerow_concat` (`types.c:3682`),
`type_typerow_union` (`types.c:3708`), and `type_typerow_intersect`
(`types.c:3758`) -- none of them reads or forwards `field_names`.

`type_eq` (`types.c:270`) and `type_typerow_eq_perm` (`types.c:3629`) both
compare `field_names` when present, so a `NULL`-named result compares equal to
any other `NULL`-named row with the same elements. Hence the false accept.

## Fix directions

1. Thread names through each operation and call `type_typerow_named()` when any
   operand carried them. For `row-canon` the names must be permuted by the same
   comparison the elements are sorted by -- and the sort key probably wants to
   become `(field_name, type_name)` rather than `type_name` alone, so that
   `#row{a : int  b : int}` has a stable canonical order at all.
2. Decide the mixed case deliberately: `(row-union #row{id : int} #row{int})`
   has one labeled and one bare operand. Rejecting it (the literal-level
   `TUR-E0290` rule, "all-or-nothing labels") is the consistent choice, and is
   cheaper than inventing names.
3. `row-intersect` needs a matching rule too -- whether two slots intersect on
   element type alone or on `(name, type)`. `(name, type)` matches the way
   labeled rows unify elsewhere.

Until then, the guide documents the limitation: keep labeled rows as literals
and do not feed them to the row algebra.
