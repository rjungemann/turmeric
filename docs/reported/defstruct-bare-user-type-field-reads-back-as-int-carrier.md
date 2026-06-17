---
title: A defstruct field typed by a bare (nullary) user type name reads back as its :int carrier, not the nominal type
severity: medium -- silent type-erasure at every field-access site. The field is stored fine, but `(.field x)` is typed `:int` instead of the declared struct/ADT/opaque, so it cannot be passed where the declared type is expected without a re-pin `(:: (.field x) :T)`. This is the exact `:int` stand-in disease CLAUDE.md dedicates a section to -- it forces callers to either ascribe at every use or (worse) declare the field `:int` up front, exporting an API the type checker can't help with.
status: open
discovered: 2026-06-17
surfaced-by: turmeric-spices ECS work (E2d). A `(defopaque Tag :int)` stored in a world struct read back as `:int` from `(.tag w)`, forcing a `(:: (.Tag w) :Tag)` re-pin at every accessor. Parameterized opaques `(Tag A)` do NOT exhibit this, which is what made the asymmetry obvious.
---

# Bare nullary user-type field reads back as `:int`, losing nominal identity

## One-line summary

When a `defstruct` field's type is a **bare symbol** that resolves to a
nullary (non-parameterized) user-defined `defstruct` / `defdata` /
`defopaque`, the elaborator records the field's storage kind as `TY_INT`
but leaves `full_type == NULL`. Every `(.field x)` access then types as
`:int` (the carrier), not the declared nominal type. Parameterized field
types (`(Tag A)`, `(Box int)`, `(Dense Pos)`) keep their `full_type` and
read back correctly, so the bug is invisible until a *nullary* user type
is used as a field.

## Minimal repro

```turmeric
(defopaque Tag :int)

(defstruct World [tag : Tag])

(defn use-tag [t : Tag] : int
  (:: t :int))

(defn main [] : int
  (let [w (make-struct World (:: 7 :Tag))]
    (use-tag (.tag w))))      ;; <-- (.tag w) is typed :int, not Tag
```

```
$ ./build/tur run /tmp/repro.tur
repro.tur:10:14: error [TUR-E0001]: function 'use-tag' arg 1: expected Tag, got int
10 |     (use-tag (.tag w))))
   |              ^^^^^^^^
```

The workaround is to re-pin at every access site:

```turmeric
(use-tag (:: (.tag w) :Tag))   ;; compiles
```

### Not opaque-specific -- regular structs/ADTs too

A plain nested struct field has the same hole:

```turmeric
(defstruct Inner [v : int])
(defstruct Outer [inner : Inner])

(defn use-inner [i : Inner] : int (.v i))

(defn main [] : int
  (let [o (make-struct Outer (make-struct Inner 7))]
    (use-inner (.inner o))))   ;; error: expected Inner, got int
```

Even nested access `(.v (.inner o))` fails -- because `(.inner o)` is
typed `:int`, the `.v` accessor has no struct receiver to dispatch on
("no typeclass method found for 'v'"). This is almost certainly *why*
the in-tree fixtures that need recursive/nested struct fields declare
them `:int` and cast manually -- e.g.
`tests/fixtures/clone-list/input.tur` (`next : int` for a self-link)
and `tests/fixtures/typeclass-poly-wrapper-struct-receiver/input.tur`
(`GameWorld [Pos : int]`). Those `:int` fields are the symptom, not a
coincidence.

### Parameterized field types are fine (the asymmetry)

```turmeric
(defopaque Tag [A] :int)
(defstruct Marker [x : int])
(defstruct World [tag : (Tag Marker)])

(defn use-tag [t : (Tag Marker)] : int (:: t :int))

(defn main [] : int
  (let [w (make-struct World (:: 7 (Tag Marker)))]
    (use-tag (.tag w))))       ;; OK -- (.tag w) reads back as (Tag Marker)
```

## Root cause

`src/compiler/elab_structs.c`. In both field-parsing branches (new-style
list fields and old-style flat vector), a bare-symbol field type that
isn't a builtin falls through to a user-type lookup:

- new-style: `elab_structs.c:772-783`
- old-style: `elab_structs.c:876-890`

```c
if (fkind == TY_UNKNOWN) {
    const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
    Binding *tb = scope_lookup_type_def(e->scope, type_sym);
    if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
        fkind = TY_INT;          /* storage is an int64_t pointer -- correct */
        finner = TY_UNKNOWN;
    } else { /* ... unrecognized type ... */ }
}
```

`fkind = TY_INT` is the right *storage* kind (any struct/ADT/opaque
lowers to an `int64_t` carrier at the C level), but `full_type` is never
set on this path, so `def->fields[fi].full_type` stays `NULL`
(`elab_structs.c:824` / `:934`).

Then at the read site, `elab_struct_field_use_type`
(`elab_structs.c:394-422`) sees `field->full_type == NULL` and falls all
the way through to:

```c
return type_from_kind(field->kind);   /* == TYPE_INT */   // elab_structs.c:422
```

so the access expression is typed `:int`. The parameterized /
compound-type paths avoid this because they set `full_type` -- via
`struct_type_has_named_tyvar` (`:768` / `:871`) or the `F_LIST` branch's
`t->kind == TY_APP` check (`:754-757` / `:857-860`).

## Proposed fix

On the `tb->type.kind == TY_STRUCT || TY_ADT` fallthrough, store the
resolved nominal type as the field's `full_type` (keeping `fkind =
TY_INT` for storage):

```c
if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
    fkind = TY_INT;
    finner = TY_UNKNOWN;
    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
    *t = tb->type;               /* nominal struct/ADT/opaque identity */
    full_type = t;
}
```

`elab_struct_field_use_type` already returns `*field->full_type` when set
(after the parameterized-instantiation guard, which is a no-op here since
`def->n_type_params == 0` for the *container* in the common case -- but
note the guard keys off the *container's* type params, so a nullary field
type inside a parameterized struct still needs checking).

### Risk / validation

The C ABI is unchanged (storage stays `int64_t`), so struct-layout
codegen should not move. The change is additive at the type level: field
accesses that were `:int` become the nominal type, so code that *relied*
on the int readback (e.g. `(:: (.f x) :int)` or arithmetic on the raw
carrier) must be reviewed. Because so much in-tree/spice code currently
declares such fields `:int` to dodge this bug, those declarations are
unaffected (they really are `:int`); only fields actually declared with a
user type name change behavior.

1. The minimal repros above compile and run without the `(:: ... :T)`
   re-pin.
2. `bash tests/run.sh` is green (watch for any fixture that leaned on the
   int readback; regenerate snapshots only if codegen genuinely moves --
   it should not, since storage is identical).
3. Add a fixture pinning both the opaque and the plain-struct nested-field
   readback.
4. Once landed, the `:int` self-link / nested-struct fields in
   `clone-list` and `typeclass-poly-wrapper-struct-receiver` can be
   tightened to their real types (separate cleanup; coordinate fixture
   regen).

## Cross-references

- CLAUDE.md "No Lazy `:int` Stand-Ins" -- this bug is the upstream
  *cause* of several such stand-ins in the fixture tree.
- `docs/reported/defopaque-struct-payload-fails-through-unsafe-helper.md`
  -- adjacent opaque/struct carrier-typing issue (different path).
- `docs/reported/quasiquote-splice-into-vector-unsupported.md` -- the
  other gap surfaced by the same E2d session.
