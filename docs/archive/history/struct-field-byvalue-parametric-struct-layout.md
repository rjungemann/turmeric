---
title: A defstruct field whose type is a by-value parametric struct is laid out as the int64 carrier, so make-struct rejects the value and .field reads a non-struct
severity: medium -- the struct-field layout straddle from defstruct-byvalue-struct-field-stored-as-int-carrier.md, on the parametric (TY_APP) side. Blocks composing the json container typeclasses (Encode/Decode [Option]) into derive-json struct fields in turmeric-spices spices/json.
status: resolved
discovered: 2026-06-21
resolved: 2026-06-21
surfaced-by: turmeric-spices spices/json derive-json, composing the already-shipped (Encode/Decode [Option]) container instances into struct fields. Same carrier-vs-concrete family as #475/#479/#480/#481, on the struct-field layout side.
---

# By-value parametric-struct defstruct field forced to the int64 carrier

## One-line summary

`elab_defstruct` lowers a field whose type is a **by-value parametric struct
application** (e.g. `(Option cstr)`, a `TY_APP`) to the int64 carrier
(`fkind = TY_INT`) for STORAGE.  But the monomorphized value
(`Option__cstr`) is an embedded by-value aggregate, not the int64 carrier --
so `make-struct` cannot initialize the slot and a `.field` read dereferences
a non-struct.  This mirrors the nullary by-value case
(defstruct-byvalue-struct-field-stored-as-int-carrier.md) but on the
parametric (`TY_APP`) side; a `:heap` container field (`(Cons int)`) already
worked because it is an int64-carried typed pointer.

## Minimal repro

```turmeric
(defstruct Box [id : int  v : (Option cstr)])
(defn main [] : int
  (let [b (make-struct Box 1 (:: (some "x") (Option cstr)))]
    (println (if (.is-some (.v b)) 1 0)))   ; expect 1
  0)
```

Observed (pre-existing), emitted-C errors:

```
error: incompatible types when initializing type 'long int' using type 'Option__cstr'
error: request for member 'is_some' in something not a structure or union
```

The field slot was emitted as `int64_t v;` rather than the embedded
`Option__cstr v;`.

## Root cause

Two layers had to agree:

1. `struct_field_storage_from_type` (`src/compiler/elab_structs.c`) mapped
   every `TY_APP` field type to the int64 carrier (`TY_INT`), even when the
   application is a concrete by-value (non-`:heap`) struct.

2. Even once the field is laid out as the inline aggregate, the
   monomorphized struct-app typedef (`Option__cstr`) is registered on-demand
   and flushed into `concrete_struct_apps`, which the final assembly writes
   *after* `early_file` (the user struct typedefs).  So `struct Box` referenced
   `Option__cstr` before its definition -> `unknown type name 'Option__cstr'`.

## Fix

- `struct_field_storage_from_type` now recognizes a concrete by-value
  parametric struct application (via `struct_field_app_is_byvalue_struct`:
  concrete codegen layout, not `:heap`, not opaque, not a transparent-int
  newtype) and stores it inline as `TY_STRUCT` with the `TY_APP` `full_type`,
  matching `type_c_name`'s `Option__cstr` lowering.  Carrier-shaped apps
  (heap containers, phantom/tyvar apps with no fixed size) still fall through
  to the int64 carrier.

- Pass 0 in `emit_program` (`src/compiler/emit_module.c`) now pre-registers
  and flushes the monomorphized struct-app typedef into `early_file`
  immediately before the embedding struct typedef -- the same precedent the
  fn-ptr field typedefs already use.  The per-instantiation `#ifndef TUR_TY_*`
  guard plus the `emitted` flag make the later `concrete_struct_apps` flush a
  no-op.

## Test

`tests/fixtures/defstruct-field-byvalue-parametric-struct/` -- a `defstruct`
with `(Option cstr)` and `(Option int)` fields, asserting `make-struct` +
`.field` read + some/none round-trip and an unaffected sibling scalar field.
