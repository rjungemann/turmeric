# Uniqueness Types — Implementation Plan (UT0–UT3)

> **Status:** UT0--UT3 complete (benchmarks deferred)
>
> **Target:** v3
>
> **Prerequisites:** Linear Types (LT0--LT4) complete; borrow checker (Phase 12) in place.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§3 Uniqueness Types, §1 Linear Types, §4 Substructural Type Systems)
> [linear-types-plan.md](linear-types-plan.md),
> [substructural-types-plan.md](substructural-types-plan.md)
>
> **Last updated:** 2026-05-16

---

## Motivation

Uniqueness types enforce the **at-most-one** reference discipline: a value of unique type has at most one live reference at any point. Unlike linear types (which enforce *exactly-once* usage), uniqueness types allow:

- **Zero references** -- the value may be dropped freely
- **One reference** -- the value is uniquely owned

This is already the informal guarantee provided by `ref<T>` and `&mut T`, but it is not expressed in the type system as a first-class concept. Making it explicit enables:

- In-place mutation without aliasing concerns (`sort!`, buffer writes)
- Compile-time alias analysis to eliminate unnecessary copies
- A clear foundation for the substructural framework (uniqueness is the aliasing axis; linearity is the usage-count axis)

| Property | Today | Goal |
|---|---|---|
| `ref<T>` unique by construction | **Enforced** (borrow checker) | Expose as `CK_UNIQUE`; `^unique` annotation |
| `&mut T` implies uniqueness | **Enforced** (borrow checker) | Align with `^unique` semantics |
| `^unique` user annotation | **Not supported** | UT0: type flag + annotation |
| Alias tracking for unique values | **Implicit** (borrow checker) | UT1: explicit alias state in elaborator |
| Unique mutable references | **Via `&mut T`** | UT2: `^unique ^mut T` combination |
| Stdlib unique types | Ad-hoc (`ref<T>`) | UT3: mutable arrays, buffers |

---

## Proposed Syntax

```clojure
;; Unique type annotation -- at most one live reference
(deftype Unique [a] ^unique a)

;; Annotation on a function parameter
(defn sort! [^unique v : (vec int)] : unit
  (in-place-quicksort v))

;; Unique mutable reference
(defn modify [^unique ^mut x : int] : unit
  (set! x (+ x 1)))
```

---

## Motivating Examples

### Example 1: In-place array sorting

```clojure
;; A unique vector can be mutated in place without copies
(defn sort! [^unique v : (vec int)] : unit
  (in-place-quicksort v))

(defn example [] : unit
  (let [v (vec/new 10)]
    (sort! v)
    (println v)))
```

### Example 2: Alias prevention

```clojure
(defn modify [^unique ^mut x : int] : unit
  (set! x (+ x 1)))

(defn bad [] : unit
  (let [x 42]
    (let [y x]       ; alias created
      (modify x))))  ; ERROR TUR_E0200: 'x' is not unique (aliased by 'y')
```

### Example 3: Unique buffer writes

```clojure
;; A write function that requires exclusive access
(defn buf-write [^unique buf : Buffer, data : cstr] : ^unique Buffer
  ...)

;; Chain of unique operations -- each step hands off ownership
(defn pipeline [^unique buf : Buffer] : ^unique Buffer
  (-> buf
    (buf-write "header")
    (buf-write "body")
    (buf-write "footer")))
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `ref<T>` | Already unique by construction; gains `CK_UNIQUE` | Direct alignment |
| `&mut T` | Implies uniqueness (borrow checker guarantees no aliases) | Align semantics; `&mut T` is `^unique` during borrow |
| `rc<T>` | Shared ownership -- explicitly non-unique | Wrapping a `^unique` value in `rc<T>` is forbidden; type checker rejects it |
| `defer` | Compatible with unique values | Defer runs after scope exit regardless |
| Linear types | Orthogonal axis: linearity = usage count; uniqueness = alias count | A value can be both `^linear` and `^unique` |
| Substructural types | Uniqueness does not map to a substructural rule directly | Treat as a separate aliasing discipline |
| Typeclasses | Unique params are compatible with typeclass dispatch | No special handling needed |
| Effects | Unique values can be passed through effect handlers | Ownership transferred into handler |

---

## Architecture

```
src/types.h        -- CopyKind (add CK_UNIQUE); Type struct
src/reader.c       -- Parse ^unique annotation
src/elab.c         -- Alias tracking; uniqueness state per symbol
src/typecheck.c    -- Uniqueness subtype relation
src/error.h/.c     -- Error codes TUR_E0200-TUR_E0249
```

---

## Phase UT0 — Uniqueness Type Foundations

**Goal:** Add `^unique` annotation and `CK_UNIQUE` to the type system.

- [x] Extend `CopyKind` in `src/types.h`:

  ```c
  typedef enum CopyKind {
      CK_COPY,      /* Bitwise duplication */
      CK_UNSIZED,   /* Unsized */
      CK_LINEAR,    /* Linear: use exactly once */
      CK_UNIQUE,    /* Unique: at most one live reference (affine; replaces CK_MOVE) */
  } CopyKind;
  ```

- [x] Parse `^unique` annotation in `src/elab.c`; symbol interned as `^unique`, consumed in `let` and `defn` param parsing
- [x] `CK_UNIQUE` values:
  - May be dropped freely (weakening allowed -- unlike linear)
  - May not be copied (no duplication)
  - May not be aliased (the uniqueness invariant)
- [x] `ref<T>` maps to `CK_UNIQUE` (replaces `CK_MOVE`; `#define CK_MOVE CK_UNIQUE` backward-compat alias)
- [x] Add `ty_is_unique` predicate helper (`src/types.h`)

---

## Phase UT1 — Uniqueness Checking

**Goal:** Track aliasing in the elaborator and emit errors when uniqueness is violated.

A binding's **alias state** is either `UNIQUE` (no live aliases) or `ALIASED` (one or more aliases exist).

- [x] Add `AliasState` to `Binding` (`src/elab.c`):

  ```c
  typedef enum AliasState {
      AS_UNIQUE,   /* No live aliases */
      AS_ALIASED,  /* One or more aliases exist */
  } AliasState;
  ```

- [x] On assignment `let y = x` where `x : ^unique T`:
  - If `x` is `CK_MOVE` / `CK_LINEAR`: transfer ownership (existing move semantics)
  - If `x` is `CK_UNIQUE` and not moved: mark `x` as `AS_ALIASED`, emit error `TUR_E0200`
- [x] On passing a `^unique` value to a function:
  - Transfers ownership; marks the source binding as consumed (equivalent to a move)
- [x] On scope exit: unique values may be dropped; no error (weakening is allowed)
- [x] Error on copying a `^unique` value: emit `TUR_E0201`
- [x] Error on wrapping a `^unique` value in `rc<T>`: emit `TUR_E0202`
- [x] `tur explain` entries for `TUR-E0200`, `TUR-E0201`, `TUR-E0202` (`src/diag.c`)

### Error codes

| Code | Message |
|---|---|
| `TUR_E0200` | Value `{name}` is not unique -- aliased by `{alias}` |
| `TUR_E0201` | Cannot copy unique value `{name}` |
| `TUR_E0202` | Cannot wrap unique value `{name}` in `rc<T>` -- shared ownership violates uniqueness |

---

## Phase UT2 — Unique Mutable References

**Goal:** Combine `^unique` with `^mut` for exclusive mutable access.

- [x] `^unique ^mut T` annotation: the value is both uniquely owned and mutable
- [x] Semantics: equivalent to `&mut T` borrow, but expressed as an ownership transfer rather than a temporary borrow
- [x] Integration with borrow checker: `&mut T` borrows are already guaranteed unique; align terminology
- [x] A `^unique ^mut T` parameter may mutate the value in place and return a new `^unique T`
- [x] Disallow creating a `^unique ^mut` reference while any `&T` or `&mut T` borrows are live

---

## Phase UT3 — Integration

**Goal:** Update the stdlib, error UX, and benchmarks.

- [x] Mark stdlib mutable types as `CK_UNIQUE`:
  - `(vec a)` mutable operations (`vec-push!`, `vec-pop!`) require `^unique ^mut (vec a)`; `vec-free` requires `^unique (vec a)` (consuming)
  - `Buffer` and `StringBuilder` not present in stdlib -- no action needed
- [x] `tur explain TUR_E0200`, `TUR_E0201` entries (implemented in UT1, `src/diag.c`)
- [x] Unique type annotation in generated docs (`tools/gendocs.py` updated to render `^`-annotations)
- [ ] Performance benchmarks: alias-analysis overhead in the elaborator
- [x] Integration tests: `unique-vec-ops` (FFI/inline-C + `^unique ^mut`); `unique-chain` (composition of `^unique ^mut` transforms)
- [x] Fix pre-existing `kind_verify_program` assertion: `type_from_kind` and `type_fn` now set `hkt_kind = KIND_STAR` in `elab.c`, `types.c`, `types.h`, `emit.c`

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Low | One new `CopyKind` variant; annotation parsing |
| Elaborator changes | Medium | `AliasState` per binding; alias tracking on assignment |
| Codegen changes | Low | No runtime representation change |
| C emission | Low | Identical to existing move semantics |
| Error messages | Low | Two new error codes; similar to existing borrow errors |

---

## Feature Flag

```sh
turc -Xunique-types myfile.tur
```

---

## Implementation Priority

**Medium** — v3, after Linear Types (LT0–LT4), before the full Substructural framework (ST0–ST3).

Uniqueness types are a small, self-contained extension to the existing ownership model. They partially overlap with the borrow checker's existing `&mut` guarantees and provide the aliasing-control axis that complements linear types' usage-count axis.

---

## Open Questions

1. **`CK_UNIQUE` vs. `CK_MOVE` distinction:** ~~`CK_MOVE` already prevents duplication (affine). `CK_UNIQUE` adds the alias-prevention guarantee. Should `CK_MOVE` be retired in favour of `CK_UNIQUE` + `CK_LINEAR` as the two ownership primitives?~~
   **Decision:** Retire `CK_MOVE`. Use `CK_UNIQUE` (affine, at-most-one alias) and `CK_LINEAR` (exactly-once usage) as the two ownership primitives. All existing `CK_MOVE` sites migrate to `CK_UNIQUE`.
2. **Unique types and `rc<T>`:** ~~`rc/clone` on an `rc<T>` wrapping a unique value must consume the unique reference and produce a non-unique `rc<T>`. Is this the intended API?~~
   **Decision:** Wrapping a `^unique` value in `rc<T>` is forbidden. The type checker must reject `rc/new` (and any `rc` constructor) applied to a `^unique` argument. No `rc/from-unique` escape hatch.
3. **Partial aliasing:** ~~Struct fields may be individually unique even if the struct is shared. Should `^unique` apply to fields, or only to top-level bindings?~~
   **Decision:** Follow Rust's model. `^unique` applies to top-level bindings only -- not struct field annotations. Field-level alias control is handled by the borrow checker via field projections (e.g. taking a `&mut` on a field splits the borrow, not the uniqueness type). No per-field `AliasState` tracking needed.

---

## Prior Art

- **Clean:** Uniqueness typing -- the canonical reference for at-most-one-alias systems
- **Rust:** `Box<T>` provides unique ownership; `&mut T` is a unique mutable borrow
- **Haskell (`Data.Unique`):** Library-level uniqueness (not type-system enforced)
- **Bluespec:** Uniqueness for hardware design

---

## References

- [Clean -- Uniqueness Typing](https://wiki.clean.cs.ru.nl/Uniqueness_typing)
- [advanced-type-system-feasibility-plan.md §3](advanced-type-system-feasibility-plan.md)
