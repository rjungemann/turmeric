# Plan: Typed type parameters for `defdata` and `defgadt`

> **Status:** Draft
> **Last Updated:** 2026-05-26
> **Type:** Compiler / Language

---

## Overview

Type parameters on `defdata` and `defgadt` are currently **untyped and
unenforced** at definition, call, and match sites.  They are stored as bare
name strings for documentation only, with no kind tracking, no type-variable
resolution in constructor fields, and no enforcement that call sites supply
consistent type arguments.  This means code like:

```clojure
(defdata Option [a]
  (None)
  (Some a))
```

compiles, but the `a` in `(Some a)` is silently lowered to `TY_INT` (opaque
`int64_t`) — the same as if the field had been written `:int`.  No diagnostic
is emitted when a call site passes a mismatched type, and pattern-match
bindings derived from `(Some ...)` arms do not carry the declared type of `a`.

`defstruct` has already solved this problem in **Phase TM0** (see
`src/compiler/elab_structs.c:132–253`, `442–699`) via `struct_field_type_from_form`,
the `field_type_params`/`field_type_param_kinds` symbol+kind arrays, and
`StructField.full_type`.  This plan brings `defdata` and `defgadt` to the same
level of type-parameter fidelity, then adds the checking that even `defstruct`
currently defers.

---

## Audit findings

### `defdata` (plain ADT)

| Dimension | Current state |
|-----------|---------------|
| Param storage | `const char **type_params` (name strings only); no `Kind` array |
| Field-type resolution | `parse_struct_field_type` keyword table; type-variable names fall through to `TY_INT` |
| `CtorField.kind` | Always a flat `TypeKind` (no `TY_TYVAR`, no `TY_APP`) |
| `field_forms` | Stored (for nested-match recovery only), not used for type checking |
| Call-site checking | None — constructor functions accept any `int64_t` |
| Match-arm bindings | Carry the flattened `TypeKind`, not the declared param type |
| Diagnostic on misuse | None |

Source: `elab_structs.c:936–1177`; comment at line 974 explicitly says "used for
documentation / future type-checking; they do not affect C codegen".

### `defgadt` (GADT)

| Dimension | Current state |
|-----------|---------------|
| Param storage | Same as `defdata` — name strings only |
| Field-type resolution at definition | `gadt_field_typekind_from_form` maps primitive names; type variables → `TY_INT` |
| Skolem matching at match sites | `gadt_build_skolem_env` creates a per-arm `SkolemEnv` from the return-type annotation; only **primitive** type args can be bound (ADT type args are silently skipped as `TY_INT`) |
| Kind annotations on type params | None — `^f` prefix is parsed but ignored for `defgadt` params |
| Call-site checking | None |
| Diagnostic on misuse | None |

Source: `elab_structs.c:1179–1560`.

### `defstruct` (struct)

`defstruct` is **more advanced** but still has gaps:

| Dimension | Current state |
|-----------|---------------|
| Param storage | `const char **type_params` + `const Symbol **field_type_params` + `Kind *field_type_param_kinds` |
| Field-type resolution | `struct_field_type_from_form` → proper `TY_TYVAR` with named type variable; `TY_APP` chains for applications |
| `StructField.full_type` | Stores the full `Type*` for compound / parameterised fields |
| Kind assignment | `struct_type_param_kind(n, idx)` assigns `KIND_ARROW` to all params when `n==1`, `KIND_ARROW2` when `n>=2` — position-based, ignores actual usage |
| Call-site checking | `elab_struct_type_extract_args` can unpack `TY_APP` chain, but no arity / kind mismatch diagnostic |
| Match-arm bindings | Partial — TY_TYVAR fields produce the named tyvar at match time |

Source: `elab_structs.c:132–253`, `442–699`, `types.h:218–235`.

### Summary

`defdata` is strictly worse than `defstruct`.  `defgadt` is marginally better
at match sites (skolem env) but still has no kind tracking.  All three share
the same simplistic/absent kind-inference deficiency.

---

## Goals

1. **TP1** — Give `defdata` and `defgadt` the same `field_type_params` /
   `field_type_param_kinds` / `field_forms`-as-full-type infrastructure that
   `defstruct` already has.  Constructor fields that reference a declared type
   parameter should resolve to `TY_TYVAR` (named), not `TY_INT`.

2. **TP2** — Store a `Type *full_field_type` (analogue of `StructField.full_type`)
   on `CtorField` for each parameterised or compound field type so that later
   passes (match elaboration, codegen, docs) can recover the full type.

3. **TP3** — Extend `gadt_build_skolem_env` to handle ADT-typed type arguments
   (not just primitive types), so that `(Tag (List int))` as a return-type
   annotation can bind the type parameter to `TY_ADT`.

4. **TP4** — Improve kind inference for all three forms: infer `KIND_ARROW` for a
   type parameter used as a type constructor (e.g., `^f` prefix or detected from
   field usage), rather than the current position-based heuristic.

5. **TP5** — Add call-site consistency checking: when a constructor is applied,
   verify that the supplied argument types are consistent with the declared type
   parameters across all fields of that constructor (unification / skolem check).

6. **TP6** — Improve match-arm binding types: bound variables in pattern arms
   should carry the type derived by applying the skolem env to the declared
   field type (`TY_TYVAR` → concrete type), not the flat `TypeKind`.

---

## Non-goals

- Full Hindley-Milner type inference (no global unification, no let-polymorphism
  beyond what is already present).
- Changing the C codegen representation: all parameterised fields remain
  `int64_t` at the C level.  Type safety is a compile-time-only property.
- Removing the `defgadt` / `defdata` distinction.

---

## Dependency on existing work

- `defstruct` TM0 infrastructure (`struct_field_type_from_form`,
  `elab_struct_type_extract_args`) is the model to follow.
- `type_expr_from_form` (`src/compiler/elab_core.c` / `elab_structs.c`) already
  handles `TY_TYVAR`, `TY_APP`, `TY_FORALL`, `TY_EXISTS` — re-use directly.
- `SkolemEnv` / `SkolemBinding` already exist; TP3 extends the binding logic.

---

## Phases

### Phase TP1 — `defdata` field types as full `Type*`

**Files:** `src/compiler/types.h`, `src/compiler/elab_structs.c`

#### Tasks

- [x] Add `Type *full_type` to `CtorField` (parallel to `StructField.full_type`).
- [x] Add `Kind *type_param_kinds` to `AdtDef` (parallel to the local
  `field_type_param_kinds` array already used in `defstruct`).
- [x] In `elab_defdata`, after parsing the `[a b ...]` type-param vector,
  allocate a `Symbol **field_type_params` (intern each name) and a
  `Kind *type_param_kinds` array (initialise to `KIND_STAR`; TP4 will refine).
- [x] Replace the `parse_struct_field_type` call in the constructor field loop
  with a call to a new `adt_field_type_from_form` helper (modelled on
  `struct_field_type_from_form`) that:
  - Checks if the form's symbol matches a declared type param → returns
    `TY_TYVAR` with the named type variable.
  - Otherwise delegates to `type_expr_from_form`.
- [x] When the resolved `Type*` is a `TY_TYVAR`, `TY_APP`, or compound type,
  store it in `CtorField.full_type`.  `CtorField.kind` stays as the flattened
  C-level storage kind (`TY_INT` for pointer-sized values).
- [x] Update `emit_module.c` / `emit.c` codegen to ignore `full_type` (it is
  an elaboration-only annotation; codegen already uses the flat `kind`).
- [x] Add / update fixtures:
  - `tests/fixtures/adt-param/` — extend existing fixture; add a case where
    a field explicitly uses the type parameter (`(Some a)`) and verify the
    output is unchanged (codegen regression).
  - `tests/fixtures/adt-param-tyvar/` — new fixture: `(defdata Pair [a b] (MkPair a b))`;
    check that the compiler does not crash and the field types in the elaborator
    carry `TY_TYVAR` with names `"a"` / `"b"`.

#### Exit criterion

All existing ADT fixtures green; `CtorField.full_type` carries a named `TY_TYVAR`
for each field that references a declared type parameter; C codegen output is
byte-for-byte identical to the pre-patch output.

---

### Phase TP2 — `defgadt` field types as full `Type*`

**Files:** `src/compiler/elab_structs.c`

Mirrors TP1 for `defgadt`.  The `defgadt` constructor loop already stashes
`ctor->field_forms[fi]` (the raw parsed form) — TP2 uses that same form to
populate `CtorField.full_type` via `adt_field_type_from_form`.

#### Tasks

- [x] Reuse `adt_field_type_from_form` from TP1 in `elab_defgadt`'s field loop.
- [x] Populate `CtorField.full_type` for every GADT constructor field that
  references a type parameter or uses a compound type.
- [x] Propagate `AdtDef.type_param_kinds` for `defgadt` (same as TP1).
- [x] Update `gadt_resolve_type_from_form` to prefer `CtorField.full_type` over
  the raw form re-parse when the field has already been resolved; this avoids
  double-parsing and ensures consistency between definition and match sites.
- [x] Add fixtures:
  - `tests/fixtures/gadt-param-tyvar/` — GADT with a type-variable field; verify
    that the match arm sees the correct `TY_TYVAR`.

#### Exit criterion

All existing GADT fixtures green; GADT constructor fields carry `TY_TYVAR` for
param-typed fields; no regression in codegen.

---

### Phase TP3 — ADT-typed skolem arguments in `gadt_build_skolem_env`

**Files:** `src/compiler/elab_structs.c`

Currently `gadt_build_skolem_env` skips any return-type argument that is not
a primitive name, defaulting the corresponding parameter to `TY_INT`.  This
means `(defgadt Tag [a] (MkTag :int : (Tag (List int))))` cannot refine `a`
to `List int`.

#### Tasks

- [x] In `gadt_build_skolem_env`'s argument loop, when `arg->tag == F_LIST`,
  attempt to resolve the head as a named ADT / struct via `scope_lookup`.  If
  found, add a `SkolemBinding { name, TY_ADT }` (or the full `Type` if the
  `SkolemBinding` struct is extended).
- [x] Optionally extend `SkolemBinding` to carry a full `Type` (not just
  `TypeKind`) to support ADT-typed params beyond `TY_INT` — this is a
  prerequisite for TP6 on GADT code.
- [x] Add fixture: `tests/fixtures/gadt-adt-skolem/` — GADT whose return type
  annotation includes an ADT type argument; verify the match arm body is
  accepted at the correct type.

#### Exit criterion

`gadt_build_skolem_env` correctly binds type parameters to ADT types, not just
primitive types.

---

### Phase TP4 — Kind inference for type parameters

**Files:** `src/compiler/elab_structs.c`, `src/compiler/kind_check.{c,h}`

The existing `struct_type_param_kind` assigns kinds by position (first param is
`KIND_ARROW` if there is only one, `KIND_ARROW2` if there are two or more).
This is wrong for structs that use the first param as a plain type and the
second as a constructor.  Phase TP4 replaces this with a simple usage-based
inference pass over the declared field types.

#### Tasks

- [x] Add `infer_type_param_kinds(AdtDef *def)` (or extend the existing
  `kind_infer_from_instances` pass in `kind_check.c`) that walks each
  constructor's `CtorField.full_type` (populated by TP1/TP2) and:
  - If a param appears directly as a `TY_TYVAR` (not applied to anything),
    its kind is `KIND_STAR`.
  - If a param appears as the `fn` side of a `TY_APP` node, its kind is
    `KIND_ARROW` (or `KIND_ARROW2` if applied twice).
  - Unify across all constructors; emit `TUR-E0012` kind-mismatch if inconsistent.
- [ ] Replace the `struct_type_param_kind` heuristic in `elab_defstruct` with
  the same pass so structs benefit too.
- [x] Propagate inferred kinds into `AdtDef.type_param_kinds` and
  `StructDef`'s equivalent.
- [x] Add fixture: `tests/fixtures/kind-inference-adt/` — ADT with a HKT param
  `^f` used as `(f int)` in a field; verify the compiler infers `KIND_ARROW`.

#### Exit criterion

Kind errors are reported as `TUR-E0012`; all existing fixtures green; inferred
kinds are stored on `AdtDef` / `StructDef`.

---

### Phase TP5 — Call-site type-argument consistency checking

**Files:** `src/compiler/elab_call.c`, `src/compiler/elab_structs.c`

Currently, when a constructor is applied (e.g., `(Some 42)`), no check is
performed that the argument types across calls to the same ADT are consistent.
This phase adds a lightweight unification check.

#### Tasks

- [x] In `elab_call.c`'s constructor-application path, when the callee is a
  `TY_FN` binding that originated from a constructor:
  - For each field `i`, if `CtorField.full_type` contains a `TY_TYVAR` named
    `p`, record the concrete type of argument `i` as the candidate binding for
    `p`.
  - If `p` already has a candidate binding from a previous argument (possible
    for multi-field constructors), check that the new candidate is compatible
    (same `TypeKind`, or both `TY_ADT` pointing to the same `AdtDef`); emit a
    type-mismatch diagnostic if not.
- [x] This check is intra-constructor (within one `(Ctor a b)` call); inter-call
  unification (ensuring `(Some 1)` and `(Some "hello")` are not mixed in the
  same `Option`) is deferred to a full unification pass (out of scope here).
- [x] Add fixture: `tests/fixtures/errors/adt-field-type-mismatch/` — constructor
  applied with an argument of the wrong type (once a type param has been bound
  by a prior field); verify diagnostic.

#### Exit criterion

At least intra-constructor type-arg consistency is checked and diagnosed.

---

### Phase TP6 — Match-arm binding types reflect type parameters

**Files:** `src/compiler/elab_structs.c` (`elab_match_arm`)

Currently, when a pattern `(Some x)` is matched, the binding for `x` is given
the flattened `TypeKind` (`TY_INT`), not the type that `a` was instantiated to
in this arm.  TP6 improves this by applying the skolem env (TP3) to the
declared `CtorField.full_type` (TP1/TP2) to derive the concrete binding type.

#### Tasks

- [ ] In the match-arm elaboration loop (`elab_structs.c` around
  `elab_match_arm` / the match-arm field-binding section), after the skolem env
  is built (for GADTs) or trivially empty (for plain ADTs with no return-type
  annotation):
  - If `CtorField.full_type` is `TY_TYVAR { name = "a" }` and the skolem env
    has `{a → TY_INT}`, bind the pattern variable to `TY_INT` rather than the
    default `TY_INT` fallback (no visible change yet, but sets the stage).
  - For plain `defdata`, apply any type arguments inferred from the scrutinee
    type (`TY_APP` chain) — this requires `elab_struct_type_extract_args`
    (or a new `elab_adt_type_extract_args` analogue) to unpack the type args
    from the scrutinee `Type`.
- [ ] Extend `elab_adt_type_extract_args` (new, analogous to
  `elab_struct_type_extract_args`) to unpack `TY_APP` chains on an ADT type.
- [ ] Add fixture: `tests/fixtures/adt-param-match-type/` — `(defdata Pair [a b] (MkPair a b))`;
  match on a `(Pair int bool)` scrutinee; verify the bound variables carry
  `TY_INT` and `TY_BOOL` respectively.

#### Exit criterion

Pattern-variable bindings in ADT match arms carry the concrete type derived
from the scrutinee's type arguments, not the flat `TY_INT` fallback.

---

## Sequencing

```
TP1 (defdata full_type)
  └─ TP2 (defgadt full_type)          -- adt_field_type_from_form reuse
       └─ TP3 (gadt skolem ADT args)  -- uses full_type + scope
            └─ TP6 (match arm types)  -- uses skolem env + full_type
TP1
  └─ TP4 (kind inference)             -- needs full_type for walking
TP1, TP2
  └─ TP5 (call-site checking)         -- needs full_type on CtorField
```

TP3 and TP4 are independent; TP5 and TP6 both depend on TP1+TP2 but not on
each other.

---

## Risk notes

- **Codegen stability.** All phases keep `CtorField.kind` as the flat C-level
  storage kind; `full_type` is elaboration-only.  Codegen must not be changed
  to read `full_type`.  Add a `/* codegen: ignore full_type */` comment when
  touching `emit_module.c`.
- **Interpreter (turi).** `src/turi/eval.c` pattern-matching uses `CtorField.kind`
  directly.  If `kind` is changed (it should not be), eval.c needs auditing.
  For now, `full_type` is additive and eval.c is unaffected.
- **`defdata` vs. `defgadt` sharing.** The `adt_field_type_from_form` helper
  introduced in TP1 should live as a file-static in `elab_structs.c` and be
  called from both `elab_defdata` and `elab_defgadt` to avoid divergence.
- **Forward-stub ADTs.** The pre-pass (`elab_toplevel.c:534`) registers forward
  stubs before constructors are parsed.  The stub has `n_type_params == 0` and
  `type_param_kinds == NULL`.  The re-elaboration path (the
  `is_forward_stub_adt` branch) must copy the newly-parsed `type_param_kinds`
  from the real elaboration into the stub's `AdtDef` before the constructor
  loop runs.

---

## Files to change

| File | Change |
|------|--------|
| `src/compiler/types.h` | Add `Type *full_type` to `CtorField`; add `Kind *type_param_kinds` to `AdtDef` |
| `src/compiler/elab_structs.c` | `adt_field_type_from_form` helper; update `elab_defdata`, `elab_defgadt`, `gadt_build_skolem_env`, match-arm binding |
| `src/compiler/elab_call.c` | TP5 call-site consistency check |
| `src/compiler/kind_check.c` | TP4 usage-based kind inference |
| `src/turi/eval.c` | Audit only — should need no changes |
| `src/compiler/emit_module.c` | Audit only — `full_type` must not affect codegen |
| `tests/fixtures/adt-param/` | Extend existing fixture |
| `tests/fixtures/adt-param-tyvar/` | New |
| `tests/fixtures/gadt-param-tyvar/` | New |
| `tests/fixtures/gadt-adt-skolem/` | New |
| `tests/fixtures/kind-inference-adt/` | New |
| `tests/fixtures/errors/adt-field-type-mismatch/` | New |
| `tests/fixtures/adt-param-match-type/` | New |

---

## Phase status

| Phase | Title | Status |
|-------|-------|--------|
| TP1 | `defdata` field types as full `Type*` | Done |
| TP2 | `defgadt` field types as full `Type*` | Done |
| TP3 | ADT-typed skolem args in `gadt_build_skolem_env` | Done |
| TP4 | Kind inference for type parameters | Done (partial — `struct_type_param_kind` replacement deferred) |
| TP5 | Call-site type-argument consistency | Done |
| TP6 | Match-arm binding types reflect type params | Open |
