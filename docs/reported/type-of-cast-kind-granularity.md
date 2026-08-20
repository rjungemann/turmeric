# type-of / cast operate at TypeKind granularity -- two struct types in any are indistinguishable

**Severity: low-medium** -- a silent wrong-type `cast` between two structs is
possible. Found in the 2026-08-20 docs audit.

## Repro

Box a `Point` into `any`, then `(cast a OtherStruct)` succeeds structurally
where it should panic; `type-of` reports `"struct"` / `"adt"` for every
struct/ADT.

## Root cause

The box tag is the payload's `TypeKind` (`TUR_TAG(tag_idx, ...)` in
src/compiler/emit_expr.c), not a per-name id.

## Fix direction

Widen the tag to a per-monomorph type id (the mangled-name intern table
already exists) and key `type-of`/`cast`/`is?` on it.

## Guides to update when fixed

- docs/guides/union-intersection-types-guide.md (type-of/cast section,
  Deferred table)
