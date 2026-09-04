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

## Resolution (2026-08-21)

Fixed along the report's fix direction, with one correction to it: the intern
table it points at (mangled monomorph C names) is the WRONG key. A carrier ADT's
monomorph C name is `int64_t`, which every carrier ADT shares -- keying on it
would have given two different ADTs the same id and reintroduced exactly the
confusion this replaces. The key is `type_name()`, which renders a TY_APP per
instantiation (`(Box int)` vs `(Box float)`), so identity is per-monomorph as
intended.

- `emit_any_type_id` (emit_module.c) interns a struct/ADT type and returns
  `1000 + index`; a primitive keeps its `TypeKind`, which is what the preamble's
  name switch and the float/bool box special cases still key on. The inject
  site, the cast target and the `is?` target all route through it, so they
  cannot disagree.
- `type-of` reports the source name (`"Point"`, `"Shape"`) via a per-program
  table. That table cannot live in the preamble -- the ids are per program --
  and it cannot be a forward-declared per-program function either, because the
  S2 split-runtime TU compiles the preamble STANDALONE and would have no
  definition to link. It is installed through a `g_tur_any_name_ext` function
  pointer from `__tur_static_init`; a standalone preamble leaves it NULL and
  answers "unknown", which is what it did for every struct before.
- `is?` needed a new `test_type` on its Expr: the node carried only a TypeKind,
  while `cast` already had the named target as its own result type.
- The INTERPRETER was updated in step (`src/turi/eval.c`), where its
  `type-of` comment explicitly said it was matching the compiled path's kind
  granularity. It reports an ADT value's ADT name (a `(Circle 5)` is a
  `"Shape"`, not a `"Circle"`), and `is?` / `cast` compare type identity.

The behaviour change is visible in three existing fixtures, all updated:
`any-box-adt` ("adt" -> "Shape"), `any-box-struct` ("struct" -> "Point"), and
`defdata-as-union` ("adt" -> "Expr"). Two new fixtures pin the point of the
change: `any-cast-type-identity` (type-of / is? / cast across two structs, two
ADTs and a primitive) and `any-cast-wrong-type-panics` (the cast that used to
succeed silently now aborts with `cast: any holds Point, not Other`).

`bash tests/run.sh` 2686 passed / 0 failed, `bash tests/run-turi.sh` 1852 / 0.

## Guides updated

- `docs/guides/union-intersection-types-guide.md` -- the status banner, the
  `type-of` bullet, a worked two-struct example, a paragraph on the id scheme,
  and the Deferred row for this item is deleted.
