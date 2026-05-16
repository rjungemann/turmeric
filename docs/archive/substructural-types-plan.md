# Substructural Types — Implementation Plan (ST0–ST3)

> **Status:** ST0, ST1, ST2, ST3 complete
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
> **Last updated:** 2026-05-16

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

- [x] Add `SubstructKind` to `src/types.h`:

  ```c
  typedef enum SubstructKind {
      SK_STRUCTURAL,  /* Default: weakening + contraction both allowed */
      SK_AFFINE,      /* No contraction: can discard, cannot duplicate */
      SK_RELEVANT,    /* No weakening: must use, can duplicate */
      SK_LINEAR,      /* No weakening, no contraction: use exactly once */
  } SubstructKind;
  ```

- [x] Add `substruct` field to `Type` (or integrate with existing `CopyKind`):
  - `CK_LINEAR` maps to `SK_LINEAR` (set explicitly in `type_lref`)
  - `CK_MOVE`/`CK_UNIQUE` remains `SK_STRUCTURAL` by default; `SK_AFFINE` set via annotation
  - `CK_COPY` maps to `SK_STRUCTURAL`
- [x] Parse `^affine` and `^relevant` annotations in `src/elab.c` (let bindings + defn params + function type forms)
- [x] Flags are **mutually exclusive**: a type has at most one substructural discipline
- [x] Default: `SK_STRUCTURAL` (existing types are unaffected; `SK_STRUCTURAL == 0`)
- [x] Add `arg_affine[]` and `arg_relevant[]` to `fn` type in `src/types.h`
- [x] Add `is_affine` and `is_relevant` fields to `Binding` in `src/expr.h`
- [x] Add `TUR_E0150_AFFINE_USED_TWICE` and `TUR_E0151_RELEVANT_DROPPED` to `src/diag.h`
- [x] Add `g_substructural_enabled` flag and `-Xsubstructural` CLI option (implies `-Xlinear`)

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

- [x] Extend `Binding` with `UsageState` (`USAGE_UNUSED`, `USAGE_USED_ONCE`, `USAGE_USED_MANY`) in `src/expr.h`
- [x] On each use of a variable (EX_VAR): transition `UsageState`; check `^affine` (TUR_E0150) and track `^relevant`
- [x] At scope exit (let, defn params, match arms): check `UNUSED` relevant bindings (TUR_E0151)
- [x] Pattern matching: relevant arm bindings checked at each arm scope exit
- [x] Move of a substructural variable: propagates `is_affine`/`is_relevant` to the new binding in let
- [x] `diag_code_to_string` / `diag_code_from_string` entries for TUR-E0150 and TUR-E0151
- [x] `-Xsubstructural` extern declared in `elab.c` (`g_substructural_enabled`)
- [x] Test fixtures: `affine-basic`, `affine-drop`, `affine-fn-param`, `relevant-basic` (happy)
- [x] Error fixtures: `errors/affine-used-twice`, `errors/relevant-dropped`, `errors/relevant-param-dropped`

### Error codes

| Code | Message |
|---|---|
| `TUR_E0150` | Affine value `{name}` used more than once |
| `TUR_E0151` | Relevant value `{name}` dropped without being used |
| (Re-use `TUR_E0100`–`TUR_E0102` from linear-types-plan.md for `SK_LINEAR`) | |

---

## Phase ST2 — Substructural Type Inference

**Goal:** Infer substructural disciplines where possible so explicit annotations are optional.

- [x] `ref<T>` is inferred as `SK_LINEAR`
  - Let bindings: `init->type.kind == TY_REF` → `is_linear = true`, `substruct = SK_LINEAR`
  - Defn params: `:ref` keyword annotation → `is_linear = true`, `substruct = SK_LINEAR`
  - Auto-drop is suppressed for linear `ref<T>` bindings; user must explicitly `(drop! r)` or move
- [x] Functions that consume a `ref<T>` without returning it are inferred as requiring `SK_LINEAR` or `SK_AFFINE` in that parameter
  - `:ref` keyword param annotation infers `SK_LINEAR` under `-Xsubstructural`
  - `(ref T)` type-expression in `F_TYPE_ANN` position infers `SK_LINEAR` under `-Xsubstructural`
- [x] A value that is always used at least once (provably) is inferred as `SK_RELEVANT` only if explicitly annotated (inference for relevant is conservative)
- [x] Inference is **local**: no interprocedural analysis required
- [x] Explicit annotation always overrides inference; inferred discipline can be widened by the programmer
- [x] Test fixtures: `substructural-ref-infer-let`, `substructural-ref-infer-param` (happy)
- [x] Error fixtures: `errors/substructural-ref-infer-let-dropped`, `errors/substructural-ref-infer-param-dropped`

---

## Phase ST3 — Integration

**Goal:** Stdlib patterns, documentation, and error UX.

- [x] Stdlib conventions document: when to use `^linear` vs. `^affine` vs. `^relevant`
      (`docs/guides/substructural-types-guide.md`)
- [x] Helper macros in `stdlib/macros.tur`:
  - `(with-resource [x (acquire)] body)` -- ensures `x` is consumed in `body`
  - `(must-use expr)` -- wraps `expr` in a relevant-typed wrapper; propagates `SK_RELEVANT`
    to the outer binding via type-based substruct propagation in `elab_let`
- [x] `tur --explain TUR-E0150`, `TUR-E0151` entries (added to `src/diag.c`)
- [x] Updated `tur --explain TUR-E0100`, `TUR-E0101`, `TUR-E0102` to reference the unified
      substructural framework
- [x] Integration tests: `substructural-all-three`, `substructural-relevant-param`,
      `must-use-basic`, `must-use-dup`, `with-resource-basic`;
      error fixtures: `must-use-dropped`, `substructural-relevant-param-dropped`
- [x] CT builtin `vec` added (`src/elab.c`) to support macro-generated binding vectors
      containing annotation symbols like `^relevant`
- [x] ST3 type-based substruct propagation in `elab_let`: when `init->type.substruct`
      is `SK_RELEVANT` or `SK_AFFINE` and `init` is not an `EX_VAR`, propagate the
      discipline to the new binding (enables `must-use` to annotate outer bindings)

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
3. **`^affine` vs. current move semantics:** ~~`CK_MOVE` already disallows duplication (affine behaviour). Should `CK_MOVE` be renamed `CK_AFFINE` for clarity, or kept separate?~~
   **Decision:** Retire `CK_MOVE`; migrate all existing sites to `CK_UNIQUE`. No `CK_AFFINE` variant in `CopyKind`. The distinction between affine (`^affine`) and unique (`^unique`) is tracked in `SubstructKind` (`SK_AFFINE` vs. the aliasing discipline), not in `CopyKind`. The user-facing `^affine` annotation sets `SK_AFFINE` in `SubstructKind`; the compiler's ownership representation uses only `CK_UNIQUE` and `CK_LINEAR`.

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
