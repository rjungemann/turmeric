# defstruct/defgadt same-name namespace collapse under defstruct-as-defadt lowering

**RESOLVED.** Under lowering a `defstruct` *is* an ADT, so structs and ADTs
share one namespace (per maintainer guidance): a later same-name `defgadt`/
`defdata` now SUPERSEDES the struct-origin ADT (the GADT wins).  Implemented via
an `AdtDef.superseded` flag set in the defgadt same-name check, an
`elab_lookup_type_by_name` rule that prefers the non-struct-origin winner (and
never returns a superseded def), and a C-emission skip for superseded defs (both
`emit_adt_typedef_and_ctors` and the emit_program inline ADT path) so the two
`tur_adt_<Name>` typedefs no longer collide.  `gadt-struct-namespace-prefer`
passes under force-lower; default suite 1863/0.  Original report retained below.

**Severity:** medium (force-lower only; blocks `gadt-struct-namespace-prefer`).
Default path unaffected.

## Summary

The MF4 design (`docs/design-mf4-struct-gadt-namespaces.md`) lets a `defstruct`
and a `defgadt` share a name: the struct lives in the struct namespace, the
GADT in the ADT namespace, and `:Name` type annotations prefer the GADT. The
`defgadt` duplicate check at `src/compiler/elab_structs.c:2690` explicitly
allows this -- when the pre-existing binding is `TY_STRUCT`, it warns and lets
the GADT coexist.

Under `defstruct-as-defadt` lowering the `defstruct` no longer registers a
`TY_STRUCT`; it lowers to a single-variant record `TY_ADT` with the same name.
The `defgadt` then finds an existing `TY_ADT` binding (not `TY_STRUCT`) and
takes the hard-error else branch:

```
defgadt: 'Stack' is already defined
```

## Affected fixtures (force-lower)

- `gadt-struct-namespace-prefer` -- `(defstruct Stack [A] ...)` +
  `(defgadt Stack [a] ...)`; the GADT should win for `:Stack`.

## Root cause

`elab_structs.c` ~2683-2704: the prefer-GADT path keys on
`existing_gadt_b->type.kind == TY_STRUCT`. A lowered struct is `TY_ADT`, so the
coexistence branch is skipped and the "already defined" error fires. The MF4
design relied on structs and ADTs occupying *separate* namespaces; lowering
collapses both into the ADT namespace, so two genuinely distinct type
definitions now contend for one name.

## Why this is not a quick point-patch

Allowing the GADT to "win" is not just relaxing the duplicate check: once the
struct is also an ADT, there cannot be two `AdtDef`s named `Stack` with
different constructor sets coexisting. The fixture happens never to *use* the
struct (only the GADT's `SEmpty`/`SPush` and `:Stack`), so simply letting the
GADT replace the lowered struct-ADT binding would unblock it -- but in the
general MF4 case the struct's own constructor and field accessors are still
expected to work, which a single shared name cannot provide once both are ADTs.

A principled fix needs one of:

1. A struct-origin flag on the lowered `AdtDef` (e.g. `from_struct_lowering`),
   plus a decision for what the GADT shadowing means for the struct's ctor /
   field-access bindings (warn + GADT replaces, accepting that the struct
   surface is no longer reachable by that name).
2. Name-mangling the lowered struct-ADT so it does not contend for the bare
   name, with `:Name` resolution rules updated to keep the MF4 prefer-GADT
   behaviour.

Both are larger than the field-access / currying / set! seam fixes landed so
far, and touch the MF4 namespace design directly. Worth its own focused pass.

## Fix direction

Mirror the `TY_STRUCT` coexistence branch for a *struct-origin* lowered
`TY_ADT`: detect it (option 1's flag), warn, and let the GADT register its
own `AdtDef` and binding, accepting that uses of the bare struct surface by
that name resolve to the GADT (matching the existing MF4 `:Name`-prefers-GADT
rule). Validate with a force-lower sweep, since the default path keeps the two
in separate namespaces and cannot exercise this.
