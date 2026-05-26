# Plan: Tagged-union representation for typed `Option` and `Result`

> **Status:** REJECTED
> **Last Updated:** 2026-05-26
> **Type:** Runtime representation / Compiler / Codegen / Stdlib
> **Depends on:** [route-b-typed-slots-plan.md](route-b-typed-slots-plan.md), [typed-slots-generic-substrate-plan.md](typed-slots-generic-substrate-plan.md), [typed-slots-gs5-compiler-support-plan.md](typed-slots-gs5-compiler-support-plan.md)

---

## Overview

GS5 has already moved `Option` and `Result` payload **field types** onto their
real type parameters, and the follow-up compiler work now supports typed helper
accessors such as:

- `unwrap`
- `ok-val`
- `err-val`

What remains blocked is the constructor side:

- `some`
- `none`
- `ok`
- `err`

The current fixed-field layouts still require an **inactive payload** to exist
in memory even when that branch is not logically present:

- `Option[A] = { bool is_some; A value; }`
- `Result[A B] = { bool is_ok; A ok_val; B err_val; }`

That was acceptable while payloads were carrier `int64_t`, because the inactive
field could be filled with `0`. It stops being sound once payloads are concrete
typed slots such as `double`, `const char *`, `Option__float`, or
`Pair__int__float`.

This plan isolates the representation change needed to finish constructor
migration without continuing to overload the GS5 helper-support plan.

---

## Problem statement

### 1. Fixed-field typed layouts force fake inactive payload values

Today the natural typed layouts for `Option` and `Result` are:

```c
typedef struct Option__T {
    bool is_some;
    T value;
} Option__T;

typedef struct Result__A__B {
    bool is_ok;
    A ok_val;
    B err_val;
} Result__A__B;
```

That means:

- `none` must still somehow initialize `value : A`
- `ok` must still somehow initialize `err_val : B`
- `err` must still somehow initialize `ok_val : A`

The old answer was "just write zero", but that is not a principled typed value
for arbitrary payloads.

### 2. Accessors and constructors now have different risk profiles

Accessor migration is relatively safe because it only reads the active typed
field:

- `unwrap` reads `.value`
- `ok-val` reads `.ok-val`
- `err-val` reads `.err-val`

Constructor migration is different because it must define the inactive branch's
storage rule. That design question is now the main remaining blocker for full
GS5 helper migration on `Option` and `Result`.

### 3. "Default inactive value" schemes are likely to be partial or fake

Several tempting approaches all have bad trade-offs:

1. **Always zero-fill the inactive payload**
   - may compile for many C types
   - does not give a meaningful language-level value
   - gets dubious for nested typed aggregates and any future non-trivial payload

2. **Invent per-type default values**
   - requires a whole "default value for every type" story
   - leaks representation policy into unrelated type-system/runtime areas

3. **Make callers supply the inactive payload**
   - preserves layout
   - makes `none`, `ok`, and `err` much worse APIs

The clean alternative is to stop storing both payloads as always-live fields.

---

## Goals

1. Let `some` / `none` and `ok` / `err` construct typed values without inventing
   fake inactive payloads.
2. Preserve typed payload slots for the active branch.
3. Keep accessor helpers straightforward and type-directed.
4. Make nested typed payloads work naturally:
   - `Option[Pair[int float]]`
   - `Result[Option[float] cstr]`
   - `Option[Result[int cstr]]`
5. Keep the scope focused on `Option` and `Result`, not a whole tagged-union
   feature for every type in the language.

## Non-goals

- Replacing the representation of `Pair` or `Cons`.
- Introducing arbitrary user-defined sum-type lowering in this plan.
- Solving every stdlib helper migration problem here.
- Reworking equality/typeclass dispatch beyond what `Option` and `Result`
  require.

---

## Recommended representation

Use a **tagged-union-style layout** for `Option` and `Result` concrete
instantiations:

### `Option[A]`

```c
typedef struct Option__T {
    bool is_some;
    union {
        T some_value;
    } as;
} Option__T;
```

Language-level field access would continue to treat the payload as
`value`/`unwrap`, but the inactive branch would no longer require a fabricated
typed payload.

### `Result[A B]`

```c
typedef struct Result__A__B {
    bool is_ok;
    union {
        A ok_val;
        B err_val;
    } as;
} Result__A__B;
```

This makes the active payload explicit and removes the need to initialize both
typed payload slots on every constructor path.

### Why this route

1. It matches the semantics of these containers better than the current
   fixed-field layout.
2. It solves constructor migration at the representation level instead of
   pushing the problem into fake defaults.
3. It scales to nested typed payloads without special cases.
4. It gives codegen a clear branch-local active field to initialize.

---

## Open design choices

### 1. Public field names vs internal union member names

The current source-level field surfaces are:

- `Option.value`
- `Result.ok-val`
- `Result.err-val`

We need to choose whether codegen:

- preserves those logical field names and rewrites them internally onto
  union members, or
- exposes an internal `.as.<member>` layout and teaches elaboration/emission
  about it.

Recommendation: **preserve current source-level names** and keep the union
   details internal to lowering/codegen.

### 2. Representation of `none`

Current `none` is effectively `0` / null-like. A tagged-union layout creates a
decision:

- keep `none` as a concrete stack/value struct with `is_some = false`, or
- preserve null-as-none compatibility and treat `Option` as pointer-backed in
  some paths.

Recommendation: **prefer concrete value representation** for typed `Option`
instances and stop relying on null-as-none for the GS5 typed path.

### 3. Representation of `err?` / `some?` helpers

These helpers are already branch predicates. They likely become simpler under a
tagged-union representation, but we need to decide whether:

- old carrier constructor paths remain supported temporarily, or
- the whole helper surface moves in one cutover.

Recommendation: allow a short compatibility layer if needed, but keep the final
state single-representation.

### 4. Equality helpers

`option-eq?` and `result-eq?` currently inspect both fixed fields. Under a
tagged union they should branch on the tag first, then compare only the active
member.

This is not conceptually hard, but it is part of the migration surface.

---

## Compiler and runtime work

### Phase TU1 — Introduce tagged-union lowering for concrete `Option` / `Result`

**Goal.** Concrete codegen for `Option[A]` and `Result[A B]` emits a tagged
union representation instead of always-live payload fields.

**Likely surfaces.**

- `src/compiler/types.c`
  - concrete type naming stays the same
  - struct/union body emission for `Option` and `Result` needs special lowering
- `src/compiler/emit_module.c`
  - typedef emission for concrete struct-apps likely needs
    `Option`/`Result`-specific layout logic
- any struct-layout helpers currently assuming every field is always live

**Acceptance tests.**

1. `emit-c` for `(Option float)` shows a bool tag plus union payload storage.
2. `emit-c` for `(Result float cstr)` shows a bool tag plus union members for
   `double` and `const char *`.

### Phase TU2 — Lower field access and helper accessors onto the active member

**Goal.** Existing helper surfaces (`unwrap`, `ok-val`, `err-val`) and direct
typed field reads still work against the new internal representation.

**Likely surfaces.**

- `src/compiler/emit_expr.c`
  - field access for these special containers may need custom emission
- possibly `src/compiler/elab_structs.c` or `elab_typeclasses.c` if field
  metadata needs to distinguish logical fields from physical storage
- `stdlib/option.tur`
- `stdlib/result.tur`

**Acceptance tests.**

1. `unwrap` on `(Option float)` yields `float`.
2. `ok-val` on `(Result float cstr)` yields `float`.
3. `err-val` on `(Result float cstr)` yields `cstr`.

### Phase TU3 — Migrate constructors onto the tagged-union representation

**Goal.** `some`, `none`, `ok`, and `err` construct typed values without fake
inactive payload initialization.

**Likely surfaces.**

- `stdlib/option.tur`
  - `some`, `none`, `some?`, `option-map`, `option-eq?`
- `stdlib/result.tur`
  - `ok`, `err`, `ok?`, `err?`, `result-map`, `result-eq?`
- emitted constructor paths in codegen for direct `make-struct` use sites if
  those still exist for these containers

**Acceptance tests.**

1. `(some 2.5)` works at `(Option float)`.
2. `(none)` can be used at `(Option float)` without inventing a fake float.
3. `(ok 3.75)` works at `(Result float cstr)`.
4. `(err "boom")` works at `(Result float cstr)` without inventing a fake
   `float`.

### Phase TU4 — Compatibility cleanup

**Goal.** Remove or fence off assumptions from the old pointer/carrier-based
implementation.

**Likely surfaces.**

- stdlib docs/examples
- generated doctests
- any fixtures still assuming null-as-none or heap-pointer container identity

**Acceptance tests.**

1. Typed helper composition works through constructors and accessors.
2. Existing typed-slots container fixtures pass using helpers rather than
   direct field reads where appropriate.

---

## Migration order

1. Land tagged-union lowering for concrete layouts.
2. Keep accessor behavior working.
3. Migrate constructors.
4. Rewrite container helper fixtures to exercise helper composition.
5. Only then consider whether any compatibility shims can be removed.

---

## Validation matrix

1. **Layout**
   - concrete `Option__float`
   - concrete `Result__float__cstr`
2. **Accessors**
   - `unwrap`
   - `ok-val`
   - `err-val`
3. **Constructors**
   - `some` / `none`
   - `ok` / `err`
4. **Composition**
   - nested typed payloads
   - constructor followed by accessor
   - equality on matching/mismatching branches

---

## Risks

1. **Special-casing leaks**
   - if `Option`/`Result` lowering is hacked into too many places, the
     representation change becomes fragile

2. **Compatibility drift**
   - code or tests relying on null/pointer behavior may need explicit
     migration rather than accidental compatibility

3. **Field-model mismatch**
   - source-level "struct fields" no longer map 1:1 onto physical C fields,
     which may require a cleaner abstraction than the current lowering assumes

---

## Recommendation

If GS5 needs full `Option` / `Result` helper migration, the next honest step is
to solve the constructor problem at the representation layer rather than keep
inventing fake inactive payload defaults.

That makes a tagged-union-style implementation the preferred route.
