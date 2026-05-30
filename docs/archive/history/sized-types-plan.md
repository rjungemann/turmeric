# Sized Types — Implementation Plan for Turmeric

> **Status:** SZ3 Complete -- runtime layer shipped.
> **Continuation:** [../../sized-types-completion-plan.md](../../sized-types-completion-plan.md)
> picks up at SZ4 (real `-Xsized-types` flag) and carries through SZ9
> (type-level size indices + static size checking).
> **Target:** v4
> **Prerequisites:** Phase 19 (Algebraic Effects) complete; HKT/HRT/GADT roadmap (v2) complete
> **Related:** [../../guides/advanced-type-system-rationale.md](../../guides/advanced-type-system-rationale.md)

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
- [x] Add `StaticInt` type for compile-time integers.
  - Syntax: `(deftype StaticInt [])`
  - Literals: `0`, `1`, `2`, etc.
- [x] Add size arithmetic operations:
  ```clojure
  (deftype Size []
    (Static : (-> int Size))
    (Add : (-> Size Size Size))
    (Mul : (-> Size Size Size)))
  ```
- [x] Add sized type constructors:
  ```clojure
  (deftype SizedVec [n : Size, a] ...)
  ```

**Artifacts:**
- `StaticInt` type in `stdlib/sized.tur`.
- Size arithmetic operations in `stdlib/sized.tur`.
- Basic sized type constructors in `stdlib/sized.tur`.
- Test fixtures: `sized-static-int`, `sized-size-arith`, `sized-vec-basic`.

---

### Phase SZ1: Sized Type Checking

**Goal:** Implement type checking for sized types.

**Tasks:**
- [x] Add size arithmetic type checking:
  - Ensure size operations are well-typed (e.g., `Add` only accepts `Size` arguments).
- [x] Implement sized type subtyping:
  - `(SizedVec 10 int)` is a subtype of `(SizedVec n int)` if `n` is statically known to be `10`.
- [x] Add size inference:
  - Infer sizes for expressions (e.g., `(SizedVec 10 int)` from a literal list of 10 elements).

**Artifacts:**
- Size predicates (`size-eq?`, `size-le?`, `size-lt?`, `size-ge?`, `size-gt?`) in `stdlib/sized.tur`.
- Size normalization (`size-normalize`) and algebraic simplification (`size-simplify`).
- Runtime subtyping assertions (`size-assert-eq!`, `size-assert-le!`, `size-compat?`).
- Size inference constructors (`sized-vec-of-1` through `sized-vec-of-4`, `sized-vec-from-list`).
- Test fixtures: `sized-sz1-predicates`, `sized-sz1-inference`, `sized-sz1-subtype`, `errors/sized-assert-fail`.
- Type arithmetic type checking: GADT type annotations on `size-add`/`size-mul` reject int args at compile time (demonstrated by `errors/sized-assert-fail`).

---

### Phase SZ2: Memory Layout

**Goal:** Use size information for memory allocation decisions.

**Tasks:**
- [x] Stack allocation for sized types when possible:
  - `sized-buf-with-stack [n f]` allocates on the stack via `alloca` and calls f with the buffer.
  - `sized-buf-compute [n]` dispatches to stack allocation when n ≤ 64 (threshold).
- [x] Heap allocation fallback for dynamic sizes:
  - `sized-buf-new` and `sized-buf-new-zeroed` use `malloc` for dynamic sizes.
  - `sized-buf-compute` falls back to heap when n > 64.
- [x] Optimize memory layout based on size information:
  - `SizedBuf` uses a single flat `int64_t *data` array (tight packing, cache-friendly).
  - Contrast: `SizedVec` allocates one struct per element (pointer-chasing, 20+ bytes/node).
  - `sized-buf-from-sized-vec` converts linked-list representation to flat array.

**Artifacts:**
- `stdlib/sized-buf.tur`: `SizedBuf` type with heap/stack allocation, bulk ops (`fill!`, `copy!`, `sum`, `min`, `max`), size integration (`sized-buf-size`), and conversion from `SizedVec`.
- Test fixtures: `sized-sz2-buf-basic`, `sized-sz2-stack-alloc`, `sized-sz2-layout`.

---

### Phase SZ3: Integration

**Goal:** Integrate sized types into the stdlib and FFI.

**Tasks:**
- [x] Add sized vectors, matrices, and bit vectors to the stdlib:
  - `SizedMatrix`: flat row-major `{ int64_t rows; int64_t cols; int64_t *data; }` with get/set/fill/row-sum/col-sum/total-sum/assert-shape!
  - `SizedBitVec`: packed `{ int64_t len; uint8_t *bits; }` with get/set!/clear!/toggle!/count/fill!/assert-len!
- [x] Integrate with FFI for C structs:
  - `ffi-point-size`, `ffi-array-size`, `ffi-struct-field-count` demonstrate carrying size annotations through Turmeric wrappers over opaque C pointers.
- [x] Add sized type error messages:
  - `sized-matrix-assert-shape!` and `sized-bitvec-assert-len!` produce descriptive messages via `require-msg!` on shape/length violations.
  - Passing `int` where `Size` is expected produces `TUR-E0001: expected <adt>, got int` (demonstrated by `errors/sized-sz3-shape-mismatch`).

**Artifacts:**
- `stdlib/sized-matrix.tur`: SizedMatrix with heap allocation, shape accessors, element access, bulk ops, shape assertion.
- `stdlib/sized-bits.tur`: SizedBitVec with packed bit storage, bit access ops, popcount, fill, length assertion.
- Test fixtures: `sized-sz3-matrix`, `sized-sz3-bitvec`, `sized-sz3-ffi`, `errors/sized-sz3-shape-mismatch`.

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
| SZ0         | 2 weeks  | None                  | Complete        |
| SZ1         | 3 weeks  | SZ0                   | Complete        |
| SZ2         | 3 weeks  | SZ1                   | Complete        |
| SZ3         | 2 weeks  | SZ2                   | Complete        |

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