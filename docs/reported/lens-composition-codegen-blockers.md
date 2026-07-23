# Struct-of-functions lens composition -- downstream codegen blockers

**Severity:** medium (front-end accepts the program; codegen still can't lower
the composed generic builder end to end). Blocks the plain `:copy`
struct-of-functions lens idiom through `compose-lens`.

**Status (2026-07-23):**
- **Blocker 1 -- RESOLVED.** Struct-name apostrophe no longer leaks into C
  identifiers (fix below; fixture
  `tests/fixtures/adt-name-apostrophe-monomorph-mangle/`).
- **Blocker 2 -- PARTIALLY RESOLVED.** It was two independent facets:
  - **Facet 2a (captured-lens env-field carrier mismatch) -- RESOLVED.** A
    generic fn returning a struct of *N* closures now specializes the env of
    *every* closure, not just the first (fix below).
  - **Facet 2b (intermediate middle-type-param erased to the int64 carrier) --
    OPEN.** This is the genuine end-to-end-monomorphization north-star; root
    cause pinpointed below.

## Context

The Trowel `lens-example.tur` (a plain `defstruct Lens' :copy [S A]` holding
`get`/`put` closures, plus a generic `compose-lens`) first tripped a spurious
`TUR-E0005` use-after-move in the front end. That was a move-checker asymmetry
and is **fixed** (enclosing-signature tyvar params in an inner closure now get
`CK_COPY` -- fixture `enclosing-tyvar-closure-param-copy/`).

With the front end passing, the program reaches codegen. A single generic
struct-of-functions instantiated at concrete types codegens and runs fine; the
blockers below are specific to a **generic function that builds** such a struct
and captures the argument lenses into the closures it constructs.

## Blocker 1 -- struct name apostrophe leaks into C identifiers -- RESOLVED

The struct is named `Lens'`. The base ADT typedef already mangled the name
(`tur_adt_Lens_`, via `mangle_field_name`), but the **monomorph** name path
spelled the raw def name straight into the C identifier:

```
struct tur_adt_Lens'__Company__Person { ... }   // invalid C -- bare apostrophe
```

Root cause: `src/compiler/types.c` built the monomorph typedef name and the
type-arg mangle suffix from the raw `def->name`:

- `type_register_adt_app` -- `buf_puts(&name, def->name)` for the base name.
- `append_type_mangle` -- `buf_puts(b, adef->name)` / `t.as.adt_.def->name` for
  an ADT/APP appearing as a type argument.

Fix: a shared `append_c_ident_mangled` helper folds any non-`[A-Za-z0-9_]` byte
to `_` (matching `mangle_field_name` / `adt_byval_c_name`) at all three
C-identifier sites, so the monomorph typedef, its ctor, and every reference
agree on `tur_adt_Lens_...`. The human-readable diagnostic printer
(`type_name_buf`) still shows the raw `Lens'`. Fixture:
`tests/fixtures/adt-name-apostrophe-monomorph-mangle/`.

## Blocker 2 -- composed-generic-struct-of-closures monomorphization

`compose-lens` captures the two argument lenses (`l1`/`l2`) into the closure
environments of the `get`/`put` it builds:

```turmeric
(defstruct Lens :copy [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defn compose-lens [S A B] [l1 : (Lens S A) l2 : (Lens A B)] : (Lens S B)
  (make-struct Lens
    (fn [s : S] : B ((. l2 get) ((. l1 get) s)))
    (fn [s : S b : B] : S ((. l1 put) s ((. l2 put) ((. l1 get) s) b)))))
```

A `:copy` struct's `make-struct` lowers to a ctor **CALL** whose arguments are
the two lifted closures. Two independent facets fell out of that shape.

### Facet 2a -- captured-lens env-field carrier mismatch -- RESOLVED

Symptom:

```
error: assigning to 'int64_t' from incompatible type 'tur_adt_Lens__Person__cstr'
    __t208->l2 = l2;
```

Root cause: the inner-closure specialization machinery
(`emit_find_passed_spec_closure` + the single `inner_closure_spec_idx` on an
`EmitAbiSpecialization`) linked **only the first** closure argument of the ctor
call. The second closure kept the base int64-carrier env struct, so the
ctor-body construction assigned a by-value monomorph struct (`l1`/`l2`) into an
`int64_t` env slot.

Fix (`emit_module.c`, `emit_expr.c`, `emit_internal.h`):
- `EmitAbiSpecialization` gains `extra_inner_closure_spec_idx[]` /
  `n_extra_inner_closure_spec_idx` (cap `TUR_EXTRA_INNER_CLOSURE_MAX`), so an
  outer spec can link **every** closure a struct-of-closures return builds.
- `emit_collect_passed_spec_closures` gathers all qualifying closures (the
  collecting sibling of `emit_find_passed_spec_closure`); the env-override
  builder is extracted into `emit_assign_inner_env_override` and reused for
  each.
- The EX_CLOSURE construction and thunk-call emit sites resolve a closure's
  inner spec via `emit_inner_closure_spec_for_binding` (primary + extras), and
  the file-scope hoist loop hoists every linked inner spec (not just the
  primary) so each suffixed env struct + drop-glue lands at file scope.

With this, all `->l1 = l1` / undefined-`__env_*__spec__` / invalid-storage-class
errors are gone; the second closure gets a proper `__env_N__spec__<...>` with
concrete field types, exactly like the first.

### Facet 2b -- intermediate middle-type-param erased to int64 carrier -- OPEN

Remaining symptom (after 2a):

```
error: incompatible type for argument 2 of
  '*(int64_t (**)(void *, tur_adt_Person)) ...'
  ... expected 'tur_adt_Person' but argument is of type 'int64_t'
```

In `((. l2 get) ((. l1 get) s))` the intermediate `((. l1 get) s)` has type `A`
(compose-lens's *middle* type param; `A = Person` at this instantiation). The
outer call's fat thunk correctly wants a by-value `tur_adt_Person`, but the
intermediate value is materialized into an `int64_t` temp.

Root cause -- **defstruct fn-field result-type erasure.** The struct field
`get : (fn [S] A)` is stored with its fn-type **result_kind collapsed to
TY_INT** (the int64 carrier), and no named `result_full_type` preserving `A`.
So the field-access-call `(. l1 get) s` elaborates with a bare `TY_INT` result,
not `TY_TYVAR A`. The outer spec *does* bind `A -> Person` (verified: the
inner-closure spec carries `S->Company, A->Person, B->cstr`), so if the field
type preserved `A` as a named tyvar, `emit_resolve_type` would spell every use
-- the materialization temp (`emit_temp_decl`/`emit_control_result_temp_decl`),
the fat-dispatch return, and the next call's arg -- as by-value `tur_adt_Person`
consistently. Because the tyvar is gone, the temp is declared `int64_t` while a
16-byte `Person` flows through it.

Fix direction: preserve fn-typed defstruct/defadt field types with their named
tyvar arg/result full-types (not just `result_kind`), so a monomorphized
field-access-call recovers the concrete type from the enclosing spec's
bindings. This is squarely the end-to-end-monomorphization territory tracked by
`docs/archive/history/van-laarhoven-*` and the monomorphization north-star plan;
it touches how every fn-typed struct field is represented and accessed, so it
wants a dedicated, carefully-scoped change rather than a fat-dispatch-site
band-aid (a band-aid recovers the thunk-cast return type but leaves the
materialization temp `int64_t`, i.e. still inconsistent).

## Minimal repros

Blocker 1 (RESOLVED): fixture
`tests/fixtures/adt-name-apostrophe-monomorph-mangle/`.

Blocker 2 (facet 2b still open): compose two concrete lenses over wide by-value
`:copy` structs and force the composed `get`/`put` to run --

```turmeric
(defstruct Person :copy [name : cstr age : int])
(defstruct Company :copy [ceo : Person])
(defstruct Lens :copy [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defn compose-lens [S A B] [l1 : (Lens S A) l2 : (Lens A B)] : (Lens S B)
  (make-struct Lens
    (fn [s : S] : B ((. l2 get) ((. l1 get) s)))
    (fn [s : S b : B] : S ((. l1 put) s ((. l2 put) ((. l1 get) s) b)))))
;; build ceo-lens : (Lens Company Person), name-lens : (Lens Person cstr),
;; then (compose-lens ceo-lens name-lens) and call its get -- cc errors on the
;; int64/tur_adt_Person intermediate (facet 2b).
```
