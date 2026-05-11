# Copy, Borrow, Move Semantics and Lifetimes: Analysis for Turmeric

> *Relevance assessment of Rust-style ownership model to Turmeric Language design*

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Concepts Overview](#concepts-overview)
3. [Rust's Model Deep Dive](#rusts-model-deep-dive)
4. [Turmeric's Current State](#turmerics-current-state)
5. [Relevance to Turmeric](#relevance-to-turmeric)
6. [Pros, Cons, and Tradeoffs](#pros-cons-and-tradeoffs)
7. [Recommendations](#recommendations)
8. [References](#references)

---

## Executive Summary

Rust's ownership system (copy/borrow/move + lifetimes) is **highly relevant** to Turmeric. Turmeric's current design already incorporates similar concepts (`ref<T>` for ownership, `ptr<T>` for raw borrows, `defer` for scope-based cleanup), but lacks **compile-time borrow checking** and **explicit lifetime tracking**.

**Key Finding:** Turmeric's Phase 5-10 roadmap (ref<T>, rc<T>, weak<T>, GC) parallels Rust's ownership model. Adopting Rust-style borrow semantics would provide memory safety guarantees without a tracing GC, but would require significant compiler complexity. The current approach (defer-driven cleanup + optional RC) is more pragmatic for a Lisp targeting C.

---

## Concepts Overview

| Concept | Rust | Turmeric (Current/Planned) | Purpose |
|---|---|---|---|
| **Ownership** | Every value has a unique owner | `ref<T>` (Phase 5) | Guarantees cleanup, prevents double-free |
| **Move** | `T` transferred, source invalidated | Implicit in `ref<T>` assignment | Avoids copy overhead, prevents use-after-move |
| **Copy** | `Copy` trait (bitwise copy) | Primitive types (int, bool) | Cheap duplication of simple values |
| **Borrow** | `&T` (immutable), `&mut T` (mutable) | `ptr<T>` (untracked) | Non-owning access to data |
| **Lifetimes** | `'a`, compiler-checked | Not implemented | Prevents dangling references |
| **RC** | `Rc<T>`, `Arc<T>` | `rc<T>` (Phase 9) | Shared ownership with ref-counting |
| **Weak** | `Weak<T>` | `weak<T>` (Phase 9) | Cycle-breaking non-owning reference |

---

## Rust's Model Deep Dive

### Ownership Rules (Rust)

1. **Each value has a single owner**
2. **When the owner goes out of scope, the value is dropped**
3. **There can only be one owner at a time** (moving transfers ownership)

```rust
let s1 = String::from("hello");  // s1 owns the String
let s2 = s1;                      // s1's ownership MOVED to s2
// println!("{}", s1);          // ERROR: use of moved value
```

### Borrowing Rules (Rust)

1. **You can have either:**
   - Any number of immutable references (`&T`)
   - Exactly one mutable reference (`&mut T`)
2. **References must always be valid** (lifetime system ensures this)

```rust
fn main() {
    let mut s = String::from("hello");
    let r1 = &s;     // OK: immutable borrow
    let r2 = &s;     // OK: multiple immutable borrows
    // let r3 = &mut s; // ERROR: cannot borrow `s` as mutable while immutable borrows exist
    println!("{} {}", r1, r2);
    let r3 = &mut s; // OK: r1 and r2 no longer used
    r3.push_str(", world!");
}
```

### Lifetimes

Lifetimes are annotations that tell the borrow checker how references relate:

```rust
// The lifetime 'a ensures that the return reference lives at least as long as 'a
fn longest<'a>(x: &'a str, y: &'a str) -> &'a str {
    if x.len() > y.len() { x } else { y }
}
```

Lifetime elision rules often make these explicit annotations unnecessary in practice.

### Key Traits

| Trait | Purpose | Example |
|---|---|---|
| `Copy` | Types that can be duplicated via bitwise copy | `i32`, `bool` |
| `Clone` | Types that can be explicitly duplicated | `String`, `Vec` |
| `Drop` | Types with custom cleanup logic | `File`, `MutexGuard` |
| `Borrow` | Types that can produce a reference to their data | `String` borrows as `&str` |

---

## Turmeric's Current State

### Phase 0-3: Complete

- **`ref<T>`**: Not yet implemented (Phase 5), but designed as unique owning handle
- **`ptr<T>`**: Raw pointer, untracked, documented as "sharp edge"
- **`defer`**: Scope-based cleanup injection
- **Closures**: Struct-backed with environment capture

### Phase 5: `ref<T>` with Move Semantics

From turmeric-plan.md:
> A `ref<T>` is `struct { T* p; }`. Constructing one calls `malloc`; the compiler injects a `defer (drop! r)` at the binding site. Move semantics: assigning a `ref` transfers ownership (source is poisoned at compile time). `@r` dereferences.

**This is essentially Rust's `Box<T>`:**
- Unique ownership
- Heap allocated
- Dropped at scope end (via `defer`)
- Move on assignment

### Phase 9 (v1): `rc<T>` + `weak<T>`

From turmeric-plan.md:
> `rc<T>`: shared owner via refcounting. `weak<T>`: non-owning observer that can be upgraded.

**This is essentially Rust's `Rc<T>` + `Weak<T>`:**
- Shared ownership
- Clones increment refcount
- Weak references don't prevent drop

### Phase 10 (v2): Bacon-Rajan Cycle Collector

Reference-counting with cycle detection (like Python's GC).

### Missing: Borrow Checking

Turmeric currently has **no compile-time borrow checking**:
- `ptr<T>` is completely untracked
- No prevention of use-after-free
- No prevention of double-mutate
- No lifetime validation for references

---

## Relevance to Turmeric

### Direct Relevance: High

| Turmeric Feature | Rust Equivalent | Relevance |
|---|---|---|
| `ref<T>` move semantics | `Box<T>` move | Direct parallel |
| `ptr<T>` | `*const T` / `*mut T` | Raw pointers, unsafe |
| `rc<T>` / `weak<T>` | `Rc<T>` / `Weak<T>` | Direct parallel |
| `defer` cleanup | `Drop` trait | Similar purpose |
| Closure env capture | Closure environment | Similar model |

### What Turmeric Could Adopt

1. **Borrow Checker with Lifetimes**
   - Add `&T` (immutable borrow) and `&mut T` (mutable borrow) syntax
   - Track reference lifetimes through elaboration
   - Prevent dangling references at compile time
   - **Complexity**: High (requires significant compiler work)

2. **Copy Traits**
   - Mark types as `Copy` (bitwise copy) vs `Move` (ownership transfer)
   - Auto-derive for simple types
   - **Complexity**: Medium

3. **Borrow Traits**
   - Allow `ptr<T>` to be checked as `&T` with lifetime annotations
   - Gradual adoption: start with checked `ptr<T>` in safe code, allow `unsafe` blocks to bypass
   - **Complexity**: High

4. **Lifetime Annotations**
   - Explicit `^'a` lifetime parameters on functions
   - Compiler verifies reference relationships
   - **Complexity**: High

### Current Gaps

| Gap | Risk | Rust Solution |
|---|---|---|
| Use-after-free with `ptr<T>` | High | Borrow checker + lifetimes |
| Double-mutate via `ptr<T>` | Medium | `&mut T` exclusive borrowing |
| Dangling references from closures | High | Lifetime annotations on closure captures |
| No validation of `ptr<T>` | Medium | Type system integration |

---

## Pros, Cons, and Tradeoffs

### Option A: Full Rust-Style Borrow Checker

**Pros:**
- ✅ Memory safety comparable to Rust
- ✅ No need for tracing GC (Bacon-Rajan becomes optional)
- ✅ Catches bugs at compile time
- ✅ Zero-cost abstractions (no runtime overhead)
- ✅ Aligns with modern systems programming expectations
- ✅ Enables fearless concurrency (if threads added later)

**Cons:**
- ❌ **Significant implementation complexity** (borrow checker is ~30% of rustc)
- ❌ **Learning curve** for Lisp users (foreign concept)
- ❌ **Verbose annotations** in complex cases
- ❌ **Compiler performance impact** (type checking overhead)
- ❌ **May conflict with Lisp idioms** (functional style, persistent data structures)
- ❌ **Debugging borrow checker errors** is notoriously difficult

**Tradeoffs:**
- ~6-12 months of focused development
- Requires expertise in type theory and compiler design
- May need to redesign parts of the elaborator
- Would delay Phase 4-8 features

**Risk:** HIGH - This is a major architectural change

---

### Option B: Gradual Borrow Checking (Recommended)

Adopt borrow semantics in stages:

**Stage 1: Checked Pointer Types (Phase 4-5)**
```clojure
;; Current: untracked
(let [p : ptr<int> (& x)] ...)

;; Proposed: checked borrow with lifetime
(let [p : &int (& x)] ... )  ;; compiler validates x outlives p
```

**Stage 2: Mutability Restrictions (Phase 5-6)**
```clojure
;; Current: allows multiple mutable ptrs
(let [p1 : ptr<int> (& x)
      p2 : ptr<int> (& x)] ...)

;; Proposed: exclusive mutable borrow
(let [p1 : &mut int (& x)] ... )  ;; error if x borrowed mutably elsewhere
```

**Stage 3: Lifetime Annotations (Phase 7-8)**
```clojure
(defn foo [^&'a int x] : &'a int x ...)  ;; explicit lifetime
```

**Pros:**
- ✅ Incremental adoption
- ✅ Can start with safe subset
- ✅ Allows `unsafe` escape hatch for complex cases
- ✅ Lower risk

**Cons:**
- ❌ Partial safety (only checked code is safe)
- ❌ More complex type system
- ❌ Still significant implementation effort

**Risk:** MEDIUM

---

### Option C: Current Approach + RC (Status Quo)

Continue with:
- `ref<T>`: unique ownership, defer-based cleanup
- `ptr<T>`: raw pointers, documented as unsafe
- `rc<T>`: shared ownership with refcounting (Phase 9)
- `weak<T>`: weak references (Phase 9)
- Bacon-Rajan: cycle collection (Phase 10)

**Pros:**
- ✅ Simple to implement
- ✅ Familiar to C programmers
- ✅ No borrow checker complexity
- ✅ Fits Lisp mental model
- ✅ Matches existing Phase 0-3 implementation

**Cons:**
- ❌ Memory safety relies on programmer discipline
- ❌ Use-after-free still possible with `ptr<T>`
- ❌ Double-mutate possible
- ❌ Dangling references possible
- ❌ RC overhead (atomic ops in multi-threaded context)

**Tradeoffs:**
- Accepts that Turmeric is a "sharp tools" language like C
- Safety through documentation and conventions
- Runtime checks optional (could add `-Dbounds-check` flags)

**Risk:** LOW - Evolutionary, not revolutionary

---

### Option D: Hybrid Approach (Recommended for Turmeric)

**Core Philosophy:** *Safe by default for common cases, escape hatch for experts*

1. **`ref<T>`** (Phase 5): Unique ownership, move semantics, scope-based drop
   - ✅ Already planned
   - Like Rust's `Box<T>` but simpler (no heap vs stack distinction)

2. **Checked `&T` / `&mut T`** (New - Phase 5.5): Optional checked borrows
   - New syntax: `&T` for immutable borrow, `&mut T` for mutable
   - Compiler tracks lifetimes within a function
   - Prevents obvious errors
   - Can be bypassed with `ptr<T>` for FFI or complex cases

3. **`ptr<T>`** remains: Raw pointer, untracked, unsafe
   - For FFI, performance-critical code
   - Documented as "you're on your own"

4. **`rc<T>` / `weak<T>`** (Phase 9): For shared ownership
   - Like Rust's `Rc<T>` / `Weak<T>`
   - Single-threaded, non-atomic refcounts

5. **Bacon-Rajan** (Phase 10): Optional cycle collector

**Pros:**
- ✅ Catches common errors (dangling refs, double mutate)
- ✅ No borrow checker complexity for simple cases
- ✅ Escape hatch for experts (`ptr<T>`)
- ✅ Familiar to both Rust and C programmers
- ✅ Incremental implementation

**Cons:**
- ❌ Not as safe as full Rust (checked borrows are optional)
- ❌ Still some complexity in type system
- ❌ Two pointer types may be confusing

**Risk:** LOW-MEDIUM

---

## Comparison Matrix

| Feature | Rust | Option A | Option B | Option C | Option D |
|---|---|---|---|---|---|
| Memory Safety | ✅✅✅ | ✅✅✅ | ✅✅ | ✅ | ✅✅ |
| Implementation Complexity | N/A | ❌❌❌ | ❌❌ | ✅✅ | ❌❌ |
| Learning Curve | N/A | ❌❌❌ | ❌❌ | ✅✅ | ❌ |
| Lisp Idiom Fit | N/A | ❌ | ✅ | ✅✅ | ✅✅ |
| FFI Compatibility | N/A | ✅ | ✅ | ✅✅ | ✅✅ |
| Compile Time | N/A | ❌ | ❌ | ✅✅ | ❌ |
| Runtime Overhead | N/A | ✅✅ | ✅✅ | ❌ (RC) | ✅✅ |
| Time to Implement | N/A | 6-12 mo | 3-6 mo | 1-2 mo | 2-4 mo |

---

## Recommendations

### For Turmeric: **Option D (Hybrid Approach)**

**Rationale:**

1. **Turmeric's target audience** is systems programmers coming from C/Lisp who value control over safety. A full Rust-style borrow checker would be a cultural mismatch.

2. **The current roadmap already works**: Phases 0-3 are complete, Phase 5 (`ref<T>`) provides ownership, Phase 9 (`rc<T>`/`weak<T>`) provides sharing. Adding borrow checking would delay shipping a usable language.

3. **Incremental safety is achievable**: Adding optional `&T`/`&mut T` types later (Phase 6-7) allows gradual adoption without breaking existing code.

4. **The `ptr<T>` / `ref<T>` / `rc<T>` hierarchy mirrors Rust's pointer hierarchy**: raw (`*`), owned (`Box`), shared (`Rc`). This is a proven model.

5. **Lifetimes are the hardest part**: Rust's lifetime system took years to mature. Turmeric can benefit from Rust's experience without reimplementing it fully.

### Specific Actions

1. **Phase 5 (`ref<T>`)**: Proceed as planned. This gives ownership semantics.

2. **Phase 6 (defmacro)**: Consider adding `&T` syntax as a reserved keyword for future borrow checking.

3. **Phase 7 (Stdlib)**: Add safe wrapper functions that use `ref<T>` internally, avoiding `ptr<T>` in public APIs.

4. **Phase 9 (`rc<T>`/`weak<T>`)**: Proceed as planned. Consider naming: `rc<T>` vs `shared<T>` vs `arc<T>` (if atomic).

5. **Post-Phase 9**: Evaluate adding optional borrow checking as a language extension (e.g., `(checked ...)` blocks or a `--strict` flag).

6. **Documentation**: Clearly document:
   - `ref<T>`: own it, drop at scope end
   - `ptr<T>`: borrow it, no tracking, your responsibility
   - `rc<T>`: share it, refcounted
   - `weak<T>`: observe it, may be dangling

### What NOT to Do

1. **Don't implement full borrow checker early**: It's a rabbit hole that will delay shipping.

2. **Don't make `&T` the default**: Turmeric's C target and FFI story require raw pointers to exist.

3. **Don't eliminate `ptr<T>`**: It's necessary for FFI and performance-critical code.

4. **Don't make lifetimes explicit initially**: Start with implicit lifetime inference within functions, add explicit annotations only if needed.

---

## Rust Resources for Further Study

- [Rust By Example: Lifetimes](https://doc.rust-lang.org/rust-by-example/scope/lifetime.html)
- [The Rust Book: Ownership](https://doc.rust-lang.org/book/ch04-00-understanding-ownership.html)
- [The Rust Book: References and Borrowing](https://doc.rust-lang.org/book/ch04-02-references-and-borrowing.html)
- [The Rust Book: Lifetimes](https://doc.rust-lang.org/book/ch10-00-generics.html#lifetimes)
- [Rust RFC: Non-lexical lifetimes](https://github.com/rust-lang/rfcs/blob/master/text/2094-nll.md) (advanced)
- [Subtyping and Variance](https://doc.rust-lang.org/nomicon/subtyping.html)

---

## Appendix: Syntax Proposals

### Option 1: Rust-like Syntax

```clojure
;; Unique ownership
(let [x (ref 42)] ...)

;; Immutable borrow (checked)
(let [p (& x)] ...)  ;; p: &int, compiler ensures x outlives p

;; Mutable borrow (checked, exclusive)
(let [p (&mut x)] ...)  ;; p: &mut int, compiler ensures x not borrowed elsewhere

;; Raw pointer (unchecked)
(let [p (ptr x)] ...)  ;; p: ptr<int>, no tracking
```

### Option 2: Keyword-based

```clojure
(let [x (ref 42)
      p (borrow x)] ...)  ;; checked immutable borrow

(let [x (ref 42)
      p (borrow-mut x)] ...)  ;; checked mutable borrow
```

### Option 3: Type Annotation

```clojure
(let [x (ref 42 : int)
      p : &int x] ...)  ;; p has type &int, referencing x
```

**Recommendation:** Option 1 (Rust-like) for familiarity, with `&` and `&mut` as prefix operators.

---

## Conclusion

Rust's copy/borrow/move semantics and lifetime system are **highly relevant** to Turmeric's design. The concepts map directly to Turmeric's planned features (`ref<T>`, `rc<T>`, `weak<T>`), but Turmeric should **not** adopt the full complexity of Rust's borrow checker initially.

**Turmeric's pragmatic path:** Ship with `ref<T>` ownership + `ptr<T>` raw pointers + `rc<T>`/`weak<T>` sharing, then add optional borrow checking later. This balances safety and simplicity while staying true to Turmeric's identity as a Lisp for systems programming.
