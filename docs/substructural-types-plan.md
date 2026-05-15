# Substructural Types — Implementation Plan (ST0–ST3)

> **Status:** Draft — Not Started
>
> **Target:** v3
>
> **Prerequisites:** Linear Types (LT0–LT4) complete; Uniqueness Types (UT0–UT3) complete.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§4 Substructural Type Systems, §1 Linear Types, §3 Uniqueness Types)
> [linear-types-plan.md](linear-types-plan.md),
> [uniqueness-types-plan.md](uniqueness-types-plan.md)
>
> **Last updated:** 2026-05-15

---

## Motivation

Substructural type systems generalise linear and uniqueness types into a single unified framework by controlling which of the three standard structural rules a type obeys:

| Structural Rule | Meaning | When violated |
|---|---|---|
| **Weakening** | A value can be discarded unused | Relevant types: value *must* be used |
| **Contraction** | A value can be duplicated freely | Linear/affine types: value cannot be duplicated |
| **Exchange** | Variable order doesn't matter | (Not restricted in Turmeric) |

Combining these gives three useful disciplines:

| Annotation | Weakening | Contraction | Meaning |
|---|---|---|---|
| `^linear` | No | No | Must be used exactly once |
| `^affine` | Yes | No | Can be discarded; cannot be duplicated |
| `^relevant` | No | Yes | Must be used; can be duplicated |

`^linear` and `^affine` are already partially supported via the borrow checker and `ref<T>` move semantics. This plan formalises all three disciplines as first-class type annotations and provides the unified elaborator infrastructure that the Linear Types and Uniqueness Types plans build upon.

| Property | Today | Goal |
|---|---|---|
| `^linear` (no weakening, no contraction) | Partial (`ref<T>` move-only) | ST0: explicit flag |
| `^affine` (no contraction) | Implicit (move semantics allow drop) | ST0: explicit annotation |
| `^relevant` (no weakening) | Not supported | ST0: explicit annotation |
| Substructural usage tracking | Implicit (borrow checker) | ST1: unified state machine |
| Inference of substructural annotations | Partial | ST2: generalise from `ref<T>` |
| Stdlib patterns for substructural types | Ad-hoc | ST3: conventions + helpers |

---

## Proposed Syntax

```clojure
;; Relevant -- must be used, may be duplicated
(defn must-use [^relevant resource : Resource] : unit
  ...)

;; Affine -- may be discarded, may not be duplicated
(defn one-shot [^affine key : EncryptionKey] : unit
  ...)

;; Linear -- must be used exactly once (no weakening, no contraction)
(defn consume [^linear fh : FileHandle] : unit
  ...)
```

---

## Motivating Examples

### Example 1: Relevant types for mandatory resource consumption

```clojure
;; A value that must be used but may be inspected multiple times
(defn process [^relevant resource : Resource] : unit
  (log resource)          ; first use (duplication allowed)
  (store resource))       ; second use

;; Error: relevant value dropped without use
(defn bad [] : unit
  (let [r (acquire-resource)]
    (do-nothing)))   ; ERROR TUR_E0151: relevant value 'r' not used
```

### Example 2: Affine types for one-time initialisation

```clojure
;; A key that may be dropped but must never be duplicated
(defn initialize [^affine key : EncryptionKey] : unit
  ...)

;; Error: affine value duplicated
(defn bad [] : unit
  (let [k (generate-key)]
    (initialize k)
    (initialize k)))  ; ERROR TUR_E0150: affine value 'k' used twice
```

### Example 3: Linear types for resource handles (recap from linear-types-plan.md)

```clojure
(defn open-file  [path : cstr]             : ^linear FileHandle)
(defn close-file [^linear fh : FileHandle] : unit)

(defn bad [] : unit
  (let [fh (open-file "data.txt")]
    (do-nothing)))  ; ERROR TUR_E0100: linear value 'fh' dropped without use
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `ref<T>` | Is `CK_LINEAR` (after linear-types-plan.md) | Direct alignment |
| `rc<T>` | Is `CK_COPY` -- shared ownership | Different discipline; non-substructural |
| `&T` / `&mut T` | Borrows are non-owning; structural rules do not apply | Orthogonal |
| Linear types | `^linear` is the strictest substructural discipline | ST0 subsumes LT0 |
| Uniqueness types | `^unique` is a separate aliasing discipline | Orthogonal axis; may combine |
| Effects | Substructural effect values (e.g. one-shot continuations) | Handled naturally |
| Typeclasses | Methods may carry substructural parameters | Method signatures must be compatible |

---

## Architecture

```
src/types.h        -- SubstructKind enum; Type struct field
src/reader.c       -- Parse ^linear, ^affine, ^relevant annotations
src/elab.c         -- Unified usage tracking; substructural state machine
src/typecheck.c    -- Subtype relation across substructural disciplines
src/error.h/.c     -- Error codes TUR_E0150-TUR_E0199
```

---

## Phase ST0 — Substructural Type Flags

**Goal:** Represent all three disciplines as explicit type flags.

- [ ] Add `SubstructKind` to `src/types.h`:

  ```c
  typedef enum SubstructKind {
      SK_STRUCTURAL,  /* Default: weakening + contraction both allowed */
      SK_AFFINE,      /* No contraction: can discard, cannot duplicate */
      SK_RELEVANT,    /* No weakening: must use, can duplicate */
      SK_LINEAR,      /* No weakening, no contraction: use exactly once */
  } SubstructKind;
  ```

- [ ] Add `substruct` field to `Type` (or integrate with existing `CopyKind`):
  - `CK_LINEAR` maps to `SK_LINEAR`
  - `CK_MOVE` maps to `SK_AFFINE` (move semantics already disallow duplication)
  - `CK_COPY` maps to `SK_STRUCTURAL`
- [ ] Parse `^affine` and `^relevant` annotations in `src/reader.c`
- [ ] Flags are **mutually exclusive**: a type has at most one substructural discipline
- [ ] Default: `SK_STRUCTURAL` (existing types are unaffected)

---

## Phase ST1 — Substructural Type Checking

**Goal:** Unified usage tracking for all three disciplines in the elaborator.

Each binding in the symbol table carries a `UsageState`:

```
UNUSED  --[first use]--> USED_ONCE  --[second use]--> USED_MANY
```

Rules per discipline:

| Discipline | Drop without use | Use twice |
|---|---|---|
| `SK_STRUCTURAL` | OK | OK |
| `SK_AFFINE` | OK | Error `TUR_E0150` |
| `SK_RELEVANT` | Error `TUR_E0151` | OK |
| `SK_LINEAR` | Error `TUR_E0100` | Error `TUR_E0101` |

- [ ] Extend symbol table entry with `UsageState` and `SubstructKind`
- [ ] On each use of a variable: transition `UsageState`; check against discipline
- [ ] At scope exit: check `UNUSED` bindings against discipline
- [ ] Pattern matching: each arm transitions independently; joins are checked for consistency
- [ ] Move of a substructural variable: transfers ownership to the new binding

### Error codes

| Code | Message |
|---|---|
| `TUR_E0150` | Affine value `{name}` used more than once |
| `TUR_E0151` | Relevant value `{name}` dropped without being used |
| (Re-use `TUR_E0100`–`TUR_E0102` from linear-types-plan.md for `SK_LINEAR`) | |

---

## Phase ST2 — Substructural Type Inference

**Goal:** Infer substructural disciplines where possible so explicit annotations are optional.

- [ ] `ref<T>` is inferred as `SK_LINEAR`
- [ ] Functions that consume a `ref<T>` without returning it are inferred as requiring `SK_LINEAR` or `SK_AFFINE` in that parameter
- [ ] A value that is always used at least once (provably) is inferred as `SK_RELEVANT` only if explicitly annotated (inference for relevant is conservative)
- [ ] Inference is **local**: no interprocedural analysis required
- [ ] Explicit annotation always overrides inference; inferred discipline can be widened by the programmer

---

## Phase ST3 — Integration

**Goal:** Stdlib patterns, documentation, and error UX.

- [ ] Stdlib conventions document: when to use `^linear` vs. `^affine` vs. `^relevant`
- [ ] Helper macros in `stdlib/macros.tur`:
  - `(with-resource [x (acquire)] body)` -- ensures `x` is consumed in `body`
  - `(must-use expr)` -- wraps `expr` in a relevant-typed wrapper
- [ ] `tur explain TUR_E0150`, `TUR_E0151` entries
- [ ] Update `tur explain TUR_E0100`, `TUR_E0101`, `TUR_E0102` to reference the unified substructural framework
- [ ] Integration tests: all three disciplines with typeclasses, effects, borrow checker, FFI

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Low | New `SubstructKind` enum; integrates with existing `CopyKind` |
| Elaborator changes | Medium | Unified `UsageState` machine; replaces ad-hoc move tracking |
| Codegen changes | Low | No runtime representation |
| C emission | Low | Identical to existing move/copy semantics |
| Error messages | Medium | Two new error codes; context (where was the value first used?) |

---

## Feature Flag

```sh
turc -Xsubstructural myfile.tur
```

Enabling `-Xsubstructural` also implicitly enables `-Xlinear` (since `SK_LINEAR` is a substructural discipline).

---

## Implementation Priority

**High** — v3, after Linear Types (LT0–LT4).

The substructural framework unifies the linear, affine, and relevant disciplines under one elaborator pass, replacing ad-hoc move-tracking logic. Implementing it after Linear Types lets the linear infrastructure serve as the foundation.

---

## Open Questions

1. **Separate annotations or unified framework?** `^linear`, `^affine`, `^relevant` as distinct keywords vs. a single `^substructural(linear)` form. Distinct keywords are more readable.
2. **Interaction with typeclass constraints:** A typeclass method declared with `^linear` parameters -- must all instances also be `^linear`? Likely yes; the instance discipline must be at least as restrictive as the class declaration.
3. **`^affine` vs. current move semantics:** `CK_MOVE` already disallows duplication (affine behaviour). Should `CK_MOVE` be renamed `CK_AFFINE` for clarity, or kept separate?

---

## Prior Art

- **Rust:** Ownership model is affine (`Box<T>` can be dropped; cannot be duplicated)
- **Linear Haskell:** `GHC -XLinearTypes` -- full substructural type system
- **Pfenning & Davies:** Judgmental reconstruction of substructural logic
- **BiblioPolis:** Substructural types for session types

---

## References

- [Pfenning & Davies -- A Judgmental Reconstruction of Modal Logic](https://www.cs.cmu.edu/~rwh/introspect/modal.pdf)
- [Linear Haskell -- linear-base](https://hackage.haskell.org/package/linear-base)
- [advanced-type-system-feasibility-plan.md §4](advanced-type-system-feasibility-plan.md)
