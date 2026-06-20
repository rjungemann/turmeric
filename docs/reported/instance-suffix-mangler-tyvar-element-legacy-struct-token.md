---
title: Instance-suffix mangler renders a named tyvar element as the legacy `<struct>` token to avoid churning every parametric instance's C symbol name
category: Typeclass instance mangling / representation-vs-identifier coupling -- rough edge (not a miscompile)
severity: Low. Cosmetic / maintainability. `build_inst_type_suffix` keys every
  emitted instance symbol, dict struct, and dict field off a string suffix
  derived from the instance head's type args. When the parametric-instance head
  element moved from a nameless null-def `TY_STRUCT` to a NAMED `TY_TYVAR` (so
  the associated-type element projection can recover it), the suffix for a TY_APP
  head arg would have changed from `<ctor>__ltstruct_gt` to `<ctor>_tyvar`,
  renaming the C symbols of ~85 *unrelated* parametric instances (Option/Result/
  Vec/... ) and churning their codegen snapshots for zero behavioural reason. To
  keep the change surgical the mangler now special-cases a `TY_TYVAR` head-arg
  back to the legacy `"<struct>"` token. It works and is uniqueness-safe, but it
  is a representation/identifier coupling smell worth retiring.
status: OPEN
---

# Instance mangler pins a named tyvar element to the legacy `<struct>` token

## One-line summary

`build_inst_type_suffix` (`src/compiler/elab_typeclasses.c`) builds the
per-instance C identifier suffix from the head type args. A parametric head
element is now a named `TY_TYVAR` (`A` in `(Dense A)`), but to avoid renaming
every existing parametric instance's emitted symbols the TY_APP arg branch
renders a `TY_TYVAR` arg as the string `"<struct>"` -- exactly what the old
nameless null-def `TY_STRUCT` produced -- rather than its honest `"tyvar"` /
actual name.

## Background -- why the tyvar got a name

Routing `defcomponent-accessors` through the parametric `StorageOps` instance
needed `(definstance StorageOps [(Dense A)] (type Elem = A) ...)` to project a
struct element by value (see
`docs/reported/storageops-parametric-instance-struct-element-carrier-collapse.md`,
now fixed). That required the head element `A` to survive as a *named* type
variable so the call site can unify `(Dense A)` against the concrete receiver
`(Dense Pos)` and ground `A -> Pos`. Previously an unknown applied-head arg was
parsed to a nameless `TY_STRUCT{def=NULL}`, which erased the name. The fix names
it (`type_tyvar_named`), the same migration the open-binder-skolems work did for
class/sig type params.

## The rough edge

`type_name` renders `TY_TYVAR -> "tyvar"` (types.c:1480) but a null-def
`TY_STRUCT -> "<struct>"` (types.c:1599). `build_inst_type_suffix`'s TY_APP arm
calls `type_name(*app.arg)` to build the element half of the suffix, so the
representation change alone would flip every parametric instance's suffix:

```
__inst_MonadError_throw_hyerror_Result__ltstruct_gt   ->   ..._Result_tyvar
dict_StorageOps_Dense__ltstruct_gt                    ->   dict_StorageOps_Dense_tyvar
```

A full `bash tests/run.sh` confirmed the blast radius: **85** fixtures FAILed
purely on "codegen mismatch", and every differing line was solely the
`__ltstruct_gt` <-> `_tyvar` rename (function bodies byte-identical). The C
identifier carries no semantic load here -- it only has to be unique per
instance and stable -- so the churn buys nothing.

### Current mitigation (in tree)

```c
/* build_inst_type_suffix, TY_APP arg branch */
const char *n = (aarg->kind == TY_TYVAR) ? "<struct>" : type_name(*aarg);
```

This pins the suffix byte-identical to the pre-change form, so the 85 snapshots
stay untouched and only the genuinely-new struct-element-projection path differs.

## Why it is a smell

- The suffix now lies: a real type variable is mangled as `<struct>`.
- It couples the *type representation* (named tyvar) to a *C-identifier
  back-compat* concern in a spot that should not care.
- A future reader extending the mangler (e.g. wanting per-element specialization
  in the suffix) has to know that `<struct>` secretly means "an abstract
  element tyvar".

## Fix directions (when churn is affordable)

In a dedicated, snapshot-regenerating change (the
`fixtures/*/expected.c` regen step from CLAUDE.md), drop the special-case and let
`build_inst_type_suffix` render the tyvar honestly. Options, cheapest first:

1. Render `TY_TYVAR` head args as `"tyvar"` (the `type_name` default) and regen
   all snapshots in the same commit. Smallest code delta; ~85 fixture regens.
2. Render the tyvar's actual *name* (`A`) for a more informative suffix, e.g.
   `Dense_A`. Slightly better diagnostics; same regen cost.

Either way the behavioural surface is unchanged (verified: with the legacy token
the parametric struct-element round-trip and all existing instances compile and
run identically), so this is pure cleanup gated only on picking a regen window
not colliding with in-flight branches.

## Note -- the link-failure manifestation is fixed; this report stays open

The `<struct>`-token suffix once also caused a *link failure*: a bounded
`[S] [(StorageOps S)]` wrapper over this single-param + associated-type shape
reconstructed the interior dispatch call as `..._Dense` while the instance was
emitted under `..._Dense__ltstruct_gt`. That desync is now resolved on the
*consumption* side -- `emit_concrete_inst_method_name` matches the parametric
instance head with a free-tyvar wildcard and returns the authoritative symbol,
so caller and impl agree on whatever spelling the mangler produces (see
`docs/archive/bounded-wrapper-assoc-type-single-param-name-desync.md`). This
report remains OPEN: the underlying *cosmetic* coupling (a real tyvar mangled
as `<struct>`) is untouched and is still the cleanup to retire when a
snapshot-regen window is affordable.

## Related

- `docs/reported/storageops-parametric-instance-struct-element-carrier-collapse.md`
  (the fix that introduced the named head tyvar).
- `src/compiler/elab_typeclasses.c` -- `build_inst_type_suffix` (the TY_APP arm),
  and the head-arg parse that now emits `type_tyvar_named`.
- `src/compiler/types.c:1480` (`TY_TYVAR -> "tyvar"`), `:1599`
  (null-def `TY_STRUCT -> "<struct>"`).
- The open-binder-skolems named-TYVAR migration (precedent for naming abstract
  type params and the "sites accept both shapes during the transition" note).
