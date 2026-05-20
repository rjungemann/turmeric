# Sized Types — Implementation Plan for Turmeric

> **Status:** Draft — Not Started
> **Target:** v4
> **Prerequisites:** Phase 19 (Algebraic Effects) complete; HKT/HRT/GADT roadmap (v2) complete
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)

---

## Executive Summary

Sized types track the **size** of data structures in the type system. This enables:
- Memory layout verification
- Stack allocation of fixed-size structures
- Type-safe embedded DSLs
- Array shape verification

---

## Decision Framework

Sized types are evaluated against the following criteria:
1. **Aligns with Turmeric's goals?** (Lisp expressiveness + systems control + zero-cost abstractions)
   ✅ Yes — Enables precise memory control and type-safe DSLs.
2. **Fits the C99 target?** (No GC, manual memory management, predictable performance)
   ✅ Yes — Maps naturally to C arrays and structs.
3. **Composes with existing features?** (Borrow checker, RC, typeclasses, effects)
   ✅ Yes — Orthogonal to ownership and effects.
4. **Compilation model complexity?** (Elaboration, codegen, C emission)
   Medium — Requires size tracking but no runtime overhead.
5. **User demand?** (Known use cases from Turmeric community)
   Medium — Useful for systems programming and embedded DSLs.
6. **Prior art?** (Other languages with similar features targeting C)
   ✅ Yes — Idris, Agda, ATS, F*, and C (const generics).

---

## Implementation Strategy

### Phase SZ0: Size Type Foundations

**Goal:** Add foundational support for static sizes and size arithmetic.

**Tasks:**
- [ ] Add `StaticInt` type for compile-time integers.
  - Syntax: `(deftype StaticInt [])`
  - Literals: `0`, `1`, `2`, etc.
- [ ] Add size arithmetic operations:
  ```clojure
  (deftype Size []
    (Static : (-> int Size))
    (Add : (-> Size Size Size))
    (Mul : (-> Size Size Size)))
  ```
- [ ] Add sized type constructors:
  ```clojure
  (deftype SizedVec [n : Size, a] ...)
  ```

**Artifacts:**
- `StaticInt` type in `stdlib/`.
- Size arithmetic operations in `stdlib/`.
- Basic sized type constructors.

---

### Phase SZ1: Sized Type Checking

**Goal:** Implement type checking for sized types.

**Tasks:**
- [ ] Add size arithmetic type checking:
  - Ensure size operations are well-typed (e.g., `Add` only accepts `Size` arguments).
- [ ] Implement sized type subtyping:
  - `(SizedVec 10 int)` is a subtype of `(SizedVec n int)` if `n` is statically known to be `10`.
- [ ] Add size inference:
  - Infer sizes for expressions (e.g., `(SizedVec 10 int)` from a literal list of 10 elements).

**Artifacts:**
- Size arithmetic type checking in elaborator.
- Sized type subtyping rules.
- Size inference for literals and expressions.

---

### Phase SZ2: Memory Layout

**Goal:** Use size information for memory allocation decisions.

**Tasks:**
- [ ] Stack allocation for sized types when possible:
  - Allocate `SizedVec` on the stack if `n` is a `StaticInt`.
- [ ] Heap allocation fallback for dynamic sizes:
  - Fall back to heap allocation if size is not statically known.
- [ ] Optimize memory layout based on size information:
  - Pack structs tightly based on size annotations.

**Artifacts:**
- Stack allocation logic in codegen.
- Heap allocation fallback.
- Optimized memory layout for sized types.

---

### Phase SZ3: Integration

**Goal:** Integrate sized types into the stdlib and FFI.

**Tasks:**
- [ ] Add sized vectors, matrices, and bit vectors to the stdlib:
  ```clojure
  (deftype Matrix [rows : StaticInt, cols : StaticInt, a] ...)
  ```
- [ ] Integrate with FFI for C structs:
  - Allow sized types to map to C arrays and structs.
- [ ] Add sized type error messages:
  - Clear error messages for size mismatches.

**Artifacts:**
- Sized vectors, matrices, and bit vectors in `stdlib/`.
- FFI integration for C structs.
- Sized type error messages.

---

## Complexity Assessment

| Aspect               | Complexity | Notes                                  |
|----------------------|------------|----------------------------------------|
| Type system changes  | Medium     | Size type representation               |
| Elaborator changes   | Medium     | Size arithmetic                        |
| Codegen changes      | High       | Memory layout decisions                |
| C emission           | Medium     | Size information in generated C        |
| Error messages       | Medium     | Size mismatch explanations              |

---

## Prior Art

- **Idris:** Sized types for dependent pattern matching.
- **Agda:** Sized types with termination checking.
- **ATS:** Sized types for memory management.
- **F\*:** Sized types for verification.
- **C (const generics):** Limited sized types.

---

## Recommendation

**✅ ACCEPT — Medium complexity, good fit for systems programming and embedded DSLs.**

Sized types provide:
1. Memory layout control.
2. Stack allocation opportunities.
3. Type-safe array operations.
4. Embedded DSL support.

**Implementation priority:** Medium (after Linear Types).

**Note:** Start with simple static sizes, then add size arithmetic and inference.

---

## Feature Flag Strategy

Sized types should be gated behind a feature flag:

| Feature      | Flag            | Default | Status          |
|--------------|-----------------|---------|-----------------|
| Sized Types  | `-Xsized-types` | Off     | Not Started     |

---

## Roadmap

| Phase       | Duration | Dependencies          | Status          |
|-------------|----------|-----------------------|-----------------|
| SZ0         | 2 weeks  | None                  | Not Started     |
| SZ1         | 3 weeks  | SZ0                   | Not Started     |
| SZ2         | 3 weeks  | SZ1                   | Not Started     |
| SZ3         | 2 weeks  | SZ2                   | Not Started     |

---

## Resolved Questions

1. **Interaction with Existing Collections:**
   - **Hybrid approach:** Sized types (e.g., `SizedVec`) will be separate but convertible to/from existing collections (e.g., `vec`). Utilities like `to-sized` and `from-sized` will be provided for interoperability.

2. **Size Inference:**
   - **Opt-in:** Size inference will require explicit annotations (e.g., `:size 10`). Inference for literals (e.g., `[1 2 3]` → `SizedVec 3 int`) will be explored as a stretch goal.

3. **Interaction with Algebraic Effects and Typeclasses:**
   - **Extend:** Introduce effects like `StackAlloc` to leverage sized types for stack allocation. Sized types will participate in typeclasses (e.g., `Functor` for `SizedVec`).

---

## Next Steps

1. Implement `StaticInt` and size arithmetic (SZ0).
2. Add sized type constructors and subtyping (SZ1).
3. Integrate with memory layout and allocation (SZ2).
4. Add stdlib support and FFI integration (SZ3).