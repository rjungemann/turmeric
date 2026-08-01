---
status: resolved
severity: medium
discovered: 2026-07-28
resolved: 2026-07-29
area: compiler (type-level rows, src/compiler/types.c + elab_types.c)
---

# Row operations discard a labeled row's field names

## Resolution (2026-07-29)

All three fix directions taken.

**1. Names threaded through every operation** (`src/compiler/types.c`).
`row-canon`, `row-union`, `row-concat`, and `row-intersect` now build their
result with `type_typerow_named()` and forward the operands' `field_names`.
Two new predicates back the caller-side rules: `type_typerow_is_labeled` and
`type_typerow_dup_field_name`.

`row-canon`'s sort key became `(field_name, type_name)` with the field name
leading, exactly as the report anticipated. This is load-bearing, not cosmetic:
`#row{a : int  b : int}` and `#row{b : int  a : int}` compare equal at every
slot on `type_name` alone, so a stable sort has nothing to reorder and the two
would canonicalise to *different* rows. Field names are unique within a row
(TUR-E0291), so the pair is a total order.

`row_push_unique` (union's dedup) now compares the `(name, type)` pair for a
labeled row -- two slots are the same slot only if they agree on both.

**2. Mixed labeled/bare rejected** (`elab_types.c`), the consistent choice the
report identified. The diagnostic reuses TUR-E0290, the literal-level
all-or-nothing rule. One refinement the report did not call out: an **empty**
row had to be treated as label-*neutral* rather than bare, or the report's own
`(row-union #row{id : int} #row{})` repro would have become an error instead of
the identity it is meant to be.

**3. `row-intersect` matches on `(name, type)`**, so `#row{id : int}` and
`#row{name : int}` intersect to the empty row rather than to `int`.

One case the report did not raise, but that threading names creates: a labeled
`row-concat` keeps duplicates outright, and a labeled `row-union` keeps two
slots sharing a name but disagreeing on type. Both yield a row with a repeated
field name that no literal could spell, so the fold's result is scanned and
reported as TUR-E0291 -- consistent with the literal-level rule rather than
silently dropping one of the two slots.

Fixtures: `tests/fixtures/hkt-row-ops-labeled/` (label preservation across all
four operations, including the same-type distinct-label canon case that pins the
sort key), plus `errors/row-canon-labeled-name-mismatch`,
`errors/row-union-labeled-name-mismatch`, `errors/row-op-mixed-labels`, and
`errors/row-op-duplicate-field-name`.

Zero fixture churn -- rows erase at codegen, so no snapshot moved.
`bash tests/run.sh`: 2407 passed, 0 failed. The "Row operations discard field
names" limitation is removed from
[row-types-guide.md](../../guides/row-types-guide.md) and replaced with a
"Labels through the algebra" section.

## Original report

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
