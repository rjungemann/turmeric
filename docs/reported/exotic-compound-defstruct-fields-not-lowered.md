# Exotic compound `defstruct` field forms still keep the struct path

**Summary:** A `defstruct` field whose type is one of the *list-form* exotic
built-in compound types -- `(forall ...)`, `(handler ...)`, `(arrow ...)`,
session types (`(Send ..)`/`(Recv ..)`/`(Choose ..)`/`(Branch ..)`/`(Rec ..)`/
`(Timeout ..)`), `(project ..)`, `(global ..)`, `(role ..)` -- does NOT lower to
a record ADT; it stays on the legacy `StructDef` path. These are the only
`defstruct` shapes left that can still produce a `StructDef`.

**Severity:** Low today / blocker for the final StructDef deletion. No defstruct
in the fixture suite or stdlib uses these forms, so there are **zero producers in
practice** (the DS-B guard is green -- see below). But the
structdef-retirement slice-5 deletion cannot remove the `StructDef` type / the
residual defstruct path until every field shape either lowers or is explicitly
rejected. These forms are the remaining gap.

**Context:** structdef-retirement plan
([docs/upcoming/structdef-retirement-plan.md](../upcoming/structdef-retirement-plan.md),
[docs/upcoming/structdef-deletion-scope.md](../upcoming/structdef-deletion-scope.md)).
B1-B4 + DS-A/A1/A3 lowered every other shape; DS-B installed a zero-producer
guard.

## Minimal repro

There is no in-tree fixture (that is the point -- these forms are unused). A
hand build reproduces it. In a **Debug** build (asserts on) this aborts via the
DS-B guard; in a Release build it silently takes the residual `StructDef` path:

```turmeric
(defeffect E [x :int] :nil)
;; a handler-typed capability field written in LIST form
(defstruct H [h (handler E)])   ;; does NOT lower -> StructDef path (DS-B assert fires in Debug)

(defn main [] : int 0)
```

Note the *keyword/leaf* spellings already lower fine -- e.g. `[ptr : lref<int>]`
(fixture `linear-lref-struct-field`), `[run : fn #fx{Emit}]` (A1). Only the
F_LIST spellings of the exotic forms are affected. `(lref T)`/`(& T)`/
`(borrow-mut T)` were the borrow family and were lowered in DS-A3.

## Root cause (file:line, `src/compiler/elab_structs.c`)

1. **The gate keeps them on the struct path.**
   `defstruct_field_type_lowerable` returns `false` for these heads, so
   `defstruct_lowers_to_adt` is false and the struct takes the residual
   `StructDef` path:
   - reject list at **elab_structs.c:833-841** (`sym_forall`/`sym_forall_u`,
     `sym_handler_type`, `sym_arrow`, `sym_session_*`, `sym_project_type`,
     `sym_global_type`, `sym_role_type`).

2. **Why they were left gated.** The record-ADT field parser resolves an F_LIST
   field type through `struct_field_type_from_form` (**elab_structs.c:1864**,
   inside `resolve_ctor_field`). That helper only routes a subset of compound
   heads to the real type elaborator (`type_expr_from_form`); anything else
   falls into a generic type-application loop that mis-parses e.g. `(lref int)`
   as `apply(lref, int)` -> `TUR-E0012 "cannot apply a type of kind '*'"`.
   - the routing dispatch is at **elab_structs.c:~343** (fn/c-fn/arrow),
     **~347-358** (DS-A3 added `lref`/`borrow-mut`; `&` via the `has_amp` path),
     and forall/exists just above. The exotic heads are NOT routed there, so
     lowering them naively hits the same `TUR-E0012` mis-parse.

3. **DS-B guard that flags it.** `elab_register_struct_def`
   (**elab_structs.c:~755**, `assert(0 && ...)`) is the single writer of
   `e->struct_defs[]`. It aborts in Debug if any defstruct still produces a
   `StructDef`. Suite is green with it live (1875/0), i.e. nothing triggers it
   today; a defstruct using an exotic list-form field would.

## Fix directions

Mirror what DS-A3 did for the borrow family. Two viable routes (the deletion
scope doc lists both):

- **Lower them (preferred, matches DS-A3).** Route each exotic head to
  `type_expr_from_form` in `struct_field_type_from_form` (next to the
  `lref`/`borrow-mut` case at ~347), drop it from the
  `defstruct_field_type_lowerable` reject list (833-841), and confirm the
  resulting `CtorField` kind/storage is sane (these forms lower to the int64
  carrier like other opaque handles -- `struct_field_storage_from_type`). Add a
  positive fixture per form actually intended to be supported as a struct field.
  Caveat: each form needs a quick check that the record-ADT field access and any
  `:copy`/linear/effect interaction behave (the borrow family needed the
  `TUR-E0102` linear diagnostic reproduced on the ADT `:copy` check at
  **elab_structs.c:~2410** -- an exotic form with its own struct-path-only
  diagnostic would need the same treatment).

- **Explicitly reject them.** If a given form is not meaningfully a struct field
  (e.g. `forall` is noted as "not a value-carrying field form"), emit a clear
  hard error at the gate/elaboration *before* the residual `StructDef` path
  allocates, and drop it from the residual path. Then the DS-B assert holds
  unconditionally and the type can be deleted.

Whichever is chosen per form, the end state DS-C/DS-D needs is: **no reachable
path registers a `StructDef`**, so `defstruct_lowers_to_adt` can become
always-true and `elab_register_struct_def` / `e->struct_defs[]` / the residual
struct field parser can be deleted.

## Verifying zero producers

Re-run the registry-writer check (the DS-B assert already does this at runtime):
build Debug and run `bash tests/run.sh` -- green means no producer is reachable
in the suite. To catch a specific case, drop a `fprintf` in
`elab_register_struct_def` and grep `tests/fixtures/*/actual.stderr` (the
*writer*, not the alloc site `elab_structs.c:~1400` else-branch -- the alloc
probe misses the pre-pass stub path at `elab_toplevel.c:~1092`).
