# Linear Types — Implementation Plan (LT0–LT4)

> **Status:** Draft — Not Started
>
> **Target:** v3
>
> **Prerequisites:** Phase 19 (Algebraic Effects) complete; borrow checker (Phase 12) and unique ownership (`ref<T>`, Phase 5) in place.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§1 Linear Types, §3 Uniqueness Types, §4 Substructural Type Systems)
>
> **Last updated:** 2026-05-15

---

## Motivation

Linear types enforce the **exactly-once** usage discipline: a value of linear type must be used exactly once. This generalises Turmeric's existing move semantics (`ref<T>`) to a first-class type-system concept. Linear types prevent:

- Dropping a value without using it (resource leaks)
- Using a value more than once (double-free, use-after-free)
- Copying a value without explicit duplication

`ref<T>` is already effectively linear (move-only), but linearity is not explicitly represented in the type system. Promoting it to a first-class annotation lets programmers express resource-safety guarantees directly in types and enables the foundation for session types and the full substructural framework.

| Property | Today | Goal |
|---|---|---|
| `ref<T>` unique ownership | **Enforced** (borrow checker) | Keep; `ref<T>` becomes `CK_UNIQUE` |
| `lref<T>` linear ownership | **Not supported** | New type; maps to `CK_LINEAR` |
| `^linear` user annotation | **Not supported** | LT0: type flag + annotation |
| Linearity consumption tracking | **Implicit** (borrow checker) | LT1: explicit symbol-level state machine |
| Linear function types | **Not supported** | LT2: `(-> ^linear a b)` |
| Linearity inference | **Partial** (for `ref<T>`) | LT3: generalise to user types |
| Stdlib resource types | **Ad-hoc** | LT4: `FileHandle`, `Socket`, `MutexGuard` |

---

## Proposed Syntax

```clojure
;; Linear type annotation on a type definition
(deftype Linear [a] ^linear a)

;; Annotation on a function parameter
(defn consume [^linear x : T] : unit ...)

;; Linear function arrow -- the argument is consumed
(deftype LinearFn [a b] (-> ^linear a b))
```

---

## Motivating Examples

### Example 1: Resource-safe file handles

```clojure
;; FileHandle must be closed exactly once
(defn open-file  [path : cstr]           : ^linear FileHandle)
(defn read-file  [^linear fh : FileHandle] : (result string IoError))
(defn close-file [^linear fh : FileHandle] : unit)

;; Correct -- fh is passed into read-file which owns and closes it
(defn read-then-close [path : cstr] : (result string IoError)
  (let [fh (open-file path)]
    (read-file fh)))     ; read-file consumes fh

;; Error -- fh already consumed by read-file
(defn double-close [path : cstr] : unit
  (let [fh (open-file path)]
    (read-file fh)
    (close-file fh)))    ; ERROR: fh already consumed
```

### Example 2: One-shot linear continuations

```clojure
(deftype LinearCont [a] ^linear (-> a unit))

(defn with-linear-cont [f : (-> (LinearCont a) b)] : b
  ...)
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `ref<T>` | Becomes `CK_UNIQUE` (at-most-one alias, drop freely) | Common case; no migration needed |
| `lref<T>` | New type; maps to `CK_LINEAR` (exactly-once) | For resource handles that must be explicitly consumed |
| `rc<T>` | Shared (non-linear) | Wrapping an `lref<T>` in `rc<T>` is forbidden; type checker rejects it |
| `&T` / `&mut T` | Borrows are non-owning | Linearity does not apply to borrows |
| `defer` | Runs after the linear value is consumed | Compatible |
| Typeclasses | Methods can accept linear params | Method signatures must match linearity |
| Algebraic effects | Linear values can be passed through effect handlers | Effect system unchanged |

---

## Architecture

The relevant source files:

```
src/types.h         -- Type struct; CopyKind enum
src/elab.c          -- Elaborator; symbol table; borrow/move checking
src/typecheck.c     -- Type checking and unification
src/error.h/.c      -- Error codes and messages
```

---

## Phase LT0 — Linear Type Foundations

**Goal:** Add `^linear` annotation to the type system and expose `CK_LINEAR` in the `CopyKind` enum.

- [ ] Extend `CopyKind` in `src/types.h`:

  ```c
  typedef enum CopyKind {
      CK_MOVE,      /* Move-only (consumed on use) */
      CK_COPY,      /* Bitwise duplication */
      CK_UNSIZED,   /* Unsized */
      CK_LINEAR,    /* Linear: must be used exactly once */
  } CopyKind;
  ```

- [ ] Parse `^linear` annotation in `src/reader.c`; attach to the `Type` node
- [ ] Store `CK_LINEAR` on the resulting `Type` in the elaborator
- [ ] `CK_LINEAR` values may not be:
  - Copied (no implicit duplication)
  - Dropped without being consumed
  - Used more than once
- [ ] Add `TY_LINEAR` predicate helper (analogous to `ty_is_move`)

---

## Phase LT1 — Linear Type Checking

**Goal:** Track consumption of linear variables in the elaborator and emit errors for linearity violations.

- [ ] Add a `consumed` flag to each linear binding in the symbol table (`SymEntry`)
- [ ] On each use of a linear variable:
  - If `consumed == true`: emit error `TUR_E0101` ("linear value used after move")
  - Otherwise: set `consumed = true`
- [ ] At scope exit, for each linear variable in the scope:
  - If `consumed == false`: emit error `TUR_E0100` ("linear value dropped without use")
- [ ] On copy of a linear variable: emit error `TUR_E0102` ("cannot copy linear value")
- [ ] Pattern matching: each pattern arm receives the linear value; exactly one arm executes — linearity is satisfied
- [ ] Move of a linear variable transfers ownership (sets `consumed = true` on the source binding, creates a fresh binding in the destination)

### Error codes

| Code | Message |
|---|---|
| `TUR_E0100` | Linear value `{name}` dropped without being consumed |
| `TUR_E0101` | Linear value `{name}` used after being moved/consumed |
| `TUR_E0102` | Cannot copy linear value `{name}` |
| `TUR_E0103` | Cannot wrap linear value `{name}` in `rc<T>` -- shared ownership violates linearity |

---

## Phase LT2 — Linear Function Types

**Goal:** Support function types where parameters are consumed.

- [ ] `(-> ^linear a b)` syntax: the `a` argument is consumed by the call
- [ ] Contravariant in linear input, covariant in output (standard arrow subtyping)
- [ ] Linear arrow is tracked through higher-order function application
- [ ] Function composition with linear types must respect the consumption chain

---

## Phase LT3 — Linear Type Propagation

**Goal:** Ensure `CK_LINEAR` propagates correctly through the type system without inference.

- [ ] `lref<T>` is always `CK_LINEAR`; `ref<T>` is always `CK_UNIQUE` -- no inference
- [ ] Explicit `^linear` annotation on a parameter or binding sets `CK_LINEAR` directly
- [ ] Linearity propagates through function signatures: a function returning `lref<T>` must be called in a consuming context
- [ ] No `--infer-linearity` flag; linearity is always explicit at definition sites

---

## Phase LT4 — Integration and Polish

**Goal:** Update the stdlib, error UX, and performance.

- [ ] Mark stdlib resource types as `lref<T>` / `CK_LINEAR`:
  - `FileHandle` (in `stdlib/io.tur`)
  - `Socket` (in `stdlib/net.tur`)
  - `MutexGuard` (in `stdlib/concurrent.tur`)
- [ ] `tur explain TUR_E0100` / `TUR_E0101` / `TUR_E0102` entries
- [ ] Linear type annotation in generated docs (`just docs`)
- [ ] Performance benchmarks: measure elaborator overhead from consumption tracking
- [ ] Integration tests: linear values with typeclasses, effects, STM, FFI

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | New `CopyKind` variant; annotation parsing |
| Elaborator changes | Medium | Per-symbol consumption state machine |
| Codegen changes | Low | No runtime representation change |
| C emission | Low | Identical to existing move semantics |
| Error messages | Medium | Three new error codes; good context needed |

---

## Feature Flag

Linear types are gated behind `-Xlinear` until stable:

```sh
turc -Xlinear myfile.tur
```

---

## Implementation Priority

**High** — after HKT/HRT (v2), before GADTs.

Linear types are the highest-value, lowest-risk advanced type-system feature for Turmeric. They are a natural formalisation of the existing `ref<T>` ownership model and provide the foundation for session types and the broader substructural framework planned for v3.

---

## Open Questions

1. **Should `ref<T>` be redefined as `CK_LINEAR`?** ~~Currently `ref<T>` is `CK_MOVE`. Renaming it would provide stronger guarantees and better documentation in error messages, but may affect existing code that relies on the `CK_MOVE` distinction.~~
   **Decision:** Split into two types. `ref<T>` maps to `CK_UNIQUE` (at-most-one alias, drop freely -- the common case). A new `lref<T>` maps to `CK_LINEAR` (exactly-once, silent drop is an error -- for resource handles like `FileHandle`, `Socket`, `MutexGuard`). `CK_MOVE` is retired (see uniqueness-types-plan.md).
2. **Annotation vs. inference?** ~~`^linear` is explicit. Should unannotated types that are provably used once be treated as linear automatically (opt-in with `--infer-linearity`)?~~
   **Decision:** Explicit only. Linearity is opt-in via `lref<T>` or `^linear` annotation. No `--infer-linearity` flag. Keeps the contract clear at definition sites and avoids surprise errors from silent inference.
3. **`rc/clone` interaction:** ~~`rc/clone` on an `rc<T>` that wraps a linear value should consume the linear value and produce a shared `rc<T>`. Exact semantics need design.~~
   **Decision:** Wrapping an `lref<T>` in `rc<T>` is forbidden. Shared ownership would break the exactly-once guarantee. The type checker rejects `rc/new` (and any `rc` constructor) applied to an `lref<T>` argument. New error `TUR_E0103`.

---

## Prior Art

- **Rust:** Ownership model is effectively linear (move semantics)
- **Linear Haskell:** `GHC -XLinearTypes` extension
- **Clean:** Uniqueness typing
- **Idris:** Linear types for resource management
- **ATS:** Linear types for memory management

---

## References

- [Linear Types Can Change the World! — Wadler](https://philipwadler.com/papers/linearity/linearity.pdf)
- [GHC Linear Types Proposal](https://github.com/ghc-proposals/ghc-proposals/blob/master/proposals/0111-linear-types.rst)
- [advanced-type-system-feasibility-plan.md §1](advanced-type-system-feasibility-plan.md)
