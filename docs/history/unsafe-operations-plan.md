# Unsafe Operations Plan

## Overview

This document outlines a strategy for handling unsafe operations in Turmeric using an effect-based approach. Unsafe operations are those that can violate memory safety, type safety, or other invariants — operations like raw pointer dereferencing, unchecked array access, type casting, or I/O that bypasses the type system.

The core idea: **treat unsafe operations as effects**. This gives us:
- Explicit tracking of where unsafe code can run
- Isolated regions for unsafe operations with controlled boundaries
- Type-level guarantees about which code is safe
- Composable safety proofs via effect polymorphism

---

## 1. The Problem

Unsafe operations are necessary for:
- FFI (foreign function interface)
- Low-level memory manipulation
- Performance-critical code that needs to bypass safety checks
- Interfacing with C libraries
- Implementing safe abstractions (e.g., a safe `Vec<T>` might use unsafe code internally)

But we need to:
1. **Prevent unsafe operations from leaking** into safe code
2. **Make unsafe regions explicit** and auditable
3. **Allow safe abstractions** to be built on top of unsafe primitives
4. **Maintain composability** — safe code calling safe code stays safe

---

## 2. Design: Unsafe as an Effect

### 2.1 The `Unsafe` Effect

We introduce a built-in effect `Unsafe` that marks operations which can violate safety invariants:

```rust
// In the type system, functions that perform unsafe operations
// carry the Unsafe effect in their effect row
val raw_ptr_deref : *T -> T @ {Unsafe}
val unchecked_cast : *T -> *U @ {Unsafe}
```

### 2.2 Safe vs Unsafe Contexts

The type system distinguishes between:
- **Safe functions**: effect row does not include `Unsafe`
- **Unsafe functions**: effect row includes `Unsafe`

```rust
// Safe: no unsafe operations possible
val add : int -> int -> int @ {}

// Unsafe: performs unsafe operations
val fast_memcpy : *void -> *void -> size -> () @ {Unsafe}
```

### 2.3 The `unsafe` Block / Handler

To perform unsafe operations, code must be wrapped in an `unsafe` handler:

```rust
// Safe wrapper around unsafe code
val safe_memcpy : ByteArray -> ByteArray -> size -> () @ {}
  = fun src dst n ->
      try_with
        (fun () -> fast_memcpy (ptr_of src) (ptr_of dst) n)
        ()
        { effc = (fun (type a) (e : a Effect.t) ->
            match e with
            | Unsafe -> Some (fun k -> 
                // Validate preconditions, then continue
                assert (n <= len src && n <= len dst);
                continue k ())
            | _ -> None) }
```

Actually, we can provide syntactic sugar:

```rust
val safe_memcpy : ByteArray -> ByteArray -> size -> () @ {}
  = fun src dst n ->
      unsafe {
        assert (n <= len src && n <= len dst);
        fast_memcpy (ptr_of src) (ptr_of dst) n
      }
```

The `unsafe { ... }` block is syntactic sugar for `try_with` with an `Unsafe` handler.

---

## 3. Type System Integration

### 3.1 Effect Rows

The effect row tracks `Unsafe` like any other effect:

```rust
// Pure function
val pure : int -> int @ {}

// Can perform I/O
val read_file : string -> string @ {IO}

// Can perform unsafe operations
val raw_load : *T -> T @ {Unsafe}

// Can perform both I/O and unsafe operations
val mmap_file : string -> *void @ {IO, Unsafe}
```

### 3.2 Unsafe Polymorphism

We need a way to mark that a function's safety depends on its arguments:

```rust
// This function is safe IF the predicate holds
val partition : (T -> bool) -> List<T> -> (List<T>, List<T>) @ {}
  where (T -> bool) @ {}

// But this needs to be unsafe
val partition_unsafe : (T -> bool @ {Unsafe}) -> List<T> -> (List<T>, List<T>) @ {Unsafe}
```

Actually, this is automatic with effect polymorphism — the effect row propagates naturally:

```rust
val map : (T -> U @ e) -> List<T> -> List<U> @ e

// If the function is unsafe, map is unsafe
val map_unsafe : (T -> U @ {Unsafe}) -> List<T> -> List<U> @ {Unsafe}

// If the function is safe, map is safe
val map_safe : (T -> U @ {}) -> List<T> -> List<U> @ {}
```

### 3.3 The `Safe` Typeclass

For abstractions that need to prove they don't perform unsafe operations:

```rust
// A typeclass for types that can be safely manipulated
typeclass Safe T where
  // Proof that operations on T are safe

// Only implement Safe for types that truly are safe
instance Safe Int where {}
instance Safe String where {}

// This function requires a Safe instance
val process_safely : T -> () @ {}
  where Safe T
```

This is more heavyweight than needed. Instead, we can use the effect system directly.

---

## 4. Safe Abstractions

The key design goal: **allow building safe abstractions on top of unsafe code**.

### 4.1 The Pattern

1. Unsafe primitive with `Unsafe` effect
2. Wrapper that checks preconditions
3. Returns a safe interface with `{} ` effect

```rust
// Unsafe primitive
val raw_array_get : *T -> int -> T @ {Unsafe}

// Safe wrapper
val array_get : Array<T> -> int -> Option<T> @ {}
  = fun arr i ->
      unsafe {
        if i < 0 || i >= len arr then None
        else Some (raw_array_get (ptr_of arr) i)
      }
```

### 4.2 Trusted vs Untrusted Code

- **Trusted code**: Code that uses `unsafe` blocks. This code is responsible for maintaining invariants.
- **Untrusted code**: Code that never uses `unsafe`. This code is guaranteed to be memory-safe and type-safe.

The boundary is explicit: every `unsafe` block is a trusted region.

### 4.3 Auditing Trusted Code

To minimize the trusted codebase:

1. **Keep `unsafe` blocks small** — each block should be a single operation with clear preconditions
2. **Centralize unsafe operations** — put them in a small number of well-audited modules
3. **Document invariants** — every `unsafe` block should have comments explaining what invariants it relies on
4. **Test thoroughly** — trusted code needs more extensive testing

---

## 5. Specific Unsafe Operations

### 5.1 Pointer Operations

```rust
// Raw pointer dereference
val ptr_deref : *T -> T @ {Unsafe}

// Raw pointer write
val ptr_write : *T -> T -> () @ {Unsafe}

// Pointer arithmetic
val ptr_add : *T -> int -> *T @ {Unsafe}
```

### 5.2 Type Casting

```rust
// Unchecked cast between types
val unsafe_cast : T -> U @ {Unsafe}

// Reinterpret bits of T as U (same size)
val reinterpret : T -> U @ {Unsafe}
  where SizeOf(T) == SizeOf(U)
```

### 5.3 Array Operations

```rust
// Unchecked array access
val array_get_unchecked : Array<T> -> int -> T @ {Unsafe}

// Unchecked array write
val array_set_unchecked : Array<T> -> int -> T -> () @ {Unsafe}
```

### 5.4 FFI

```rust
// Call a C function
val c_call : (*args -> *ret @ C) -> args -> ret @ {Unsafe, IO}

// Load a symbol from a dynamic library
val dlsym : *void -> string -> *void @ {Unsafe}
```

### 5.5 Memory Management

```rust
// Allocate raw memory
val malloc : size -> *void @ {Unsafe}

// Free raw memory
val free : *void -> () @ {Unsafe}

// Reallocate
val realloc : *void -> size -> *void @ {Unsafe}
```

---

## 6. Implementation Strategy

### 6.1 Phase 1: Effect System (Prerequisite)

This plan depends on the effect system from `effects-plan.md`. We need:
- Effect rows in the type system
- `try_with` and `perform` primitives
- Effect polymorphism

### 6.2 Phase 2: Unsafe Effect

1. Add `Unsafe` as a built-in effect
2. Add `unsafe { ... }` syntactic sugar for `try_with` with Unsafe handler
3. Add compiler checks that `unsafe` blocks are properly contained

### 6.3 Phase 3: Unsafe Primitives

Implement the unsafe primitives from §5, all with `Unsafe` effect:
- Pointer operations
- Type casting
- Unchecked array access
- FFI primitives
- Memory management

### 6.4 Phase 4: Safe Standard Library

Build safe wrappers around all unsafe primitives:
- `Array<T>` with bounds-checked access
- `Vec<T>` with safe growth/shrink
- Safe FFI wrappers
- Safe memory management (RC, GC, etc.)

### 6.5 Phase 5: Linting and Tooling

1. **Unsafe code linter**: warn on large `unsafe` blocks, suggest splitting
2. **Unsafe code coverage**: track what percentage of code is trusted
3. **Unsafe code documentation**: require docs for all `unsafe` blocks
4. **Fuzzing**: automatically fuzz trusted code

---

## 7. Alternatives Considered

### 7.1 Rust-Style `unsafe` Functions

Rust marks functions as `unsafe fn`, meaning the caller must ensure preconditions:

```rust
unsafe fn deref_raw_ptr(ptr: *const T) -> T { ... }

// Caller must ensure ptr is valid
let x = unsafe { deref_raw_ptr(ptr) };
```

**Pros**: Simple, explicit at call sites
**Cons**: Burden on caller to ensure safety, harder to compose

### 7.2 Haskell-Style `IO` Monad

Treat unsafe as a monad that must be contained:

```haskell
unsafe :: (() -> a) -> Unsafe a

runUnsafe :: Unsafe a -> a
```

**Pros**: Clear containment, composable
**Cons**: Monadic style is verbose, doesn't integrate with effect system

### 7.3 Clean-Style Uniqueness Types

Use uniqueness typing to prevent aliasing of mutable state:

**Pros**: Strong guarantees, no runtime overhead
**Cons**: Complex type system, doesn't handle all unsafe operations

### 7.4 Why Effects Win

The effect-based approach:
- **Integrates** with the existing effect system
- **Composes** naturally with other effects (IO, State, etc.)
- **Tracks** unsafe operations through the type system
- **Allows** safe abstractions on top of unsafe primitives
- **Matches** the existing Turmeric architecture

---

## 8. Open Questions

1. **Should `Unsafe` be a single effect or multiple?** (e.g., `UnsafeMemory`, `UnsafeIO`, `UnsafeFFI`)
   - Single: simpler, but less precise
   - Multiple: more precise, but more complex

2. **How do we handle `Unsafe` in combination with delimited continuations?**
   - Capturing a continuation that contains `Unsafe` operations
   - Reifying continuations that have performed unsafe operations

3. **Should there be a way to "seal" a value as safe?**
   - e.g., `val seal_safe : T @ {Unsafe} -> T @ {}` — requires proof
   - Useful for building safe abstractions

4. **How do we document the invariants for `unsafe` blocks?**
   - Special comment syntax?
   - Separate proof files?
   - Integrated into the type system?

5. **Should the compiler insert runtime checks at `unsafe` boundaries?**
   - e.g., automatically check pointer validity
   - Trade-off: safety vs performance

---

## 9. Recommendation

**Proceed with the effect-based approach** outlined in this document:

1. It integrates cleanly with the existing effect system design
2. It provides explicit tracking of unsafe operations
3. It allows building safe abstractions
4. It matches Turmeric's architectural direction

**Priority**: This should be implemented alongside or shortly after the core effect system, as it's a critical piece for building a practical, safe standard library with access to low-level operations.

**Next steps**:
- [ ] Finalize effect system design (from `effects-plan.md`)
- [ ] Add `Unsafe` effect to the type system
- [ ] Implement `unsafe` syntactic sugar
- [ ] Implement core unsafe primitives
- [ ] Build safe standard library wrappers
