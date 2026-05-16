# Lazy Sequences Plan for Turmeric

**Status:** Not started. LZ0–LZ3 planned.

**Prerequisites:** Phase 2 (closures), Phase 15 (typeclasses), Phase 17 (exceptions).

**Last updated:** 2026-05-14

---

## Summary

This document proposes adding lazy sequences to Turmeric. Lazy sequences are potentially infinite collections that compute values on-demand, enabling efficient chaining of transformations without intermediate allocations. The design prioritizes zero-allocation iteration, C99 compatibility, and integration with Turmeric's existing type system and borrow checking.

---

## Motivation

### Problems with Strict Collections

Turmeric's current collections (`vec`, `list`) are strict — all elements are computed eagerly:

| Collection | Evaluation | Memory | Use Case |
|---|---|---|---|
| `vec` | Strict | O(n) contiguous | Random access, fixed size |
| `list` | Strict | O(n) linked | Sequential access |
| *Lazy seq* | *On-demand* | *O(1) per step* | *Infinite, chained ops* |

Strict evaluation prevents:

1. **Infinite collections**: Cannot represent streams like `(range)` or `(repeat x)`
2. **Efficient chaining**: `(map f (filter p (map g xs)))` allocates intermediate collections
3. **Short-circuiting**: Cannot stop early in a transformation pipeline
4. **Memory efficiency**: Large datasets require full materialization

### Use Cases

1. **Infinite sequences**: `(naturals)`, `(fibonacci)`, `(cycle [1 2 3])`
2. **Efficient pipelines**: `(->> data (filter p) (map f) (take 10))` — only computes first 10 matching elements
3. **Lazy I/O**: Line-by-line file reading without loading entire file
4. **Combinatorial generation**: Cartesian products, permutations without full materialization
5. **Event streams**: Processing sequences of events as they arrive

---

## Design

### Guiding Principles

1. **Zero-cost iteration**: Consuming a lazy sequence must be as fast as a hand-written loop
2. **No hidden allocations**: Each step can allocate at most one element's worth
3. **C99 compatibility**: Must compile to standard C99 with no dependencies
4. **Borrow checker friendly**: Lifetimes must be explicit and checkable
5. **Interoperable**: Must work with existing `vec`, `list`, and FFI
6. **Composable**: Transformations must chain efficiently without materialization

### Core Abstraction: The Iterator

All lazy sequences are based on an iterator protocol. An iterator is a stateful value that can produce the next element or signal completion.

```lisp
;; Iterator typeclass - the minimal interface
(defclass Iterator [a]
  (next [self] : (option a)))
```

This is deliberately minimal. More operations (peek, clone, size-hint) can be added via separate typeclasses.

### Sequence Type

A `Seq` is a value that can be converted into an iterator:

```lisp
(defstruct Seq [iter_fn : (fn [] : (option (Iterator a)))])
```

This allows sequences to be:
- Infinite (iter_fn always returns a new iterator)
- Finite (iter_fn returns an iterator that eventually returns `none`)
- Reusable (iter_fn can return fresh iterators each time)
- Single-use (iter_fn returns the same iterator each time)

---

## Approach Evaluation

### Approach A: Iterator Typeclass (Recommended)

**Design:** Define an `Iterator` typeclass with a `next` method. Sequences are values that produce iterators.

```lisp
(defclass Iterator [a]
  (next [self] : (option a)))

defn iter-of [x :a] : (Iterator a)
  {:next (fn [self] (some x))}

defn range-iter [start :int step :int] : (Iterator int)
  {:next (fn [self]
           (let [^mut current start]
             (set! start (+ start step))
             (some (- current step))))}

defn seq-map [f : (-> a b) s : (Seq a)] : (Seq b)
  {:iter_fn (fn []
              (let [inner (iter_fn s)]
                {:next (fn [self]
                         (match (Iterator/next inner)
                           (some x) (some (f x))
                           none none))}))})
```

**Pros:**
- Minimal interface — just one method
- Extensible via typeclasses (e.g., `Peekable`, `Reversible`)
- Works with borrow checker — iterator ownership is explicit
- Zero-allocation for simple cases (closure captures are stack-allocated where possible)
- Familiar pattern from Rust, Swift, Kotlin
- Easy to implement `into-vec`, `into-list` for materialization

**Cons:**
- Requires typeclass support (already exists in Turmeric)
- Slightly more verbose than built-in syntax
- Each transformation adds a closure layer

**Performance:** O(1) per step, no intermediate allocations for chained operations

**Complexity:** Low — ~500 lines of stdlib code


### Approach B: Generator Functions (Coroutines)

**Design:** Use delimited continuations or algebraic effects to implement generator functions that `yield` values.

```lisp
(defeffect Yield [a] :bool)

defn range-gen [start :int end :int] : (-> (Generator int))
  (fn []
    (let [^mut i start]
      (while (< i end)
        (perform (Yield i))
        (set! i (+ i 1))))))

defn collect [g : (Generator a)] : (vec a)
  (let [^mut result (vec-new)]
    (handle (g)
      (Yield [x k] (vec-push! result x)
                    (resume k true))
      (Return [v] result))
    result)
```

**Pros:**
- Familiar syntax for users of Python/JS generators
- Can express complex control flow naturally
- Resumeable generators (can interleave multiple)
- Leverages existing effects infrastructure

**Cons:**
- **Significant overhead**: Each `yield` captures a continuation (expensive)
- **Memory intensive**: Continuations capture entire stack frames
- Cannot easily chain transformations without materialization
- Harder to reason about performance
- Serialization of continuations is complex

**Performance:** O(n) per yield in time and space (continuation capture)

**Complexity:** High — requires deep integration with effects system

**Verdict:** Not suitable for performance-critical sequence operations. Better for async or complex control flow, not for efficient data pipelines.


### Approach C: Church-Encoded Streams

**Design:** Represent sequences as functions that accept a consumer (fold-style).

```lisp
(defalias Stream<a> (-> (-> a :unit) :unit))

defn stream-of [x :a] : (Stream a)
  (fn [consume]
    (consume x))

defn stream-map [f : (-> a b) s : (Stream a)] : (Stream b)
  (fn [consume]
    (s (fn [x] (consume (f x)))))

defn stream-take [n :int s : (Stream a)] : (vec a)
  (let [^mut result (vec-new)
        ^mut count 0]
    (s (fn [x]
         (when (< count n)
           (vec-push! result x)
           (set! count (+ count 1)))))
    result)
```

**Pros:**
- Mathematically elegant (category-theoretic)
- Zero overhead for transformations — just function composition
- No intermediate data structures
- Easy to prove properties about

**Cons:**
- **Cannot short-circuit**: Consumer must process all elements to know when to stop
- **Hard to use**: Awkward syntax for most operations
- **No random access**: Cannot peek at next element without consuming it
- **No reuse**: Stream can only be consumed once
- Cannot express `take` without materialization

**Performance:** O(1) per transformation, but O(n) to get first element if stream is long

**Complexity:** Medium — requires careful design to be usable

**Verdict:** Interesting theoretically but impractical for most use cases. The inability to short-circuit is a dealbreaker.


### Approach D: Built-in Lazy Syntax (Compiler Support)

**Design:** Add built-in lazy list syntax with compiler-level support for desugaring.

```lisp
;; Built-in lazy list literal
(def lazy-nats (lazy-cons 1 (lazy-map inc lazy-nats)))

;; Or with dedicated syntax
(def lazy-nats (lazy 1 (map inc lazy-nats)))

;; Compiler desugars to state machine
```

**Pros:**
- Could be extremely efficient with custom codegen
- Clean syntax for common patterns
- Can optimize specific cases (e.g., `map` fusion)

**Cons:**
- **Large scope**: Requires compiler changes, not just stdlib
- **Less flexible**: Hard to add new sequence types
- **Complexity**: State machine generation is non-trivial
- **Maintenance burden**: More compiler code to maintain
- Diverges from "small core, large stdlib" philosophy

**Performance:** Could be excellent with optimization

**Complexity:** High — requires changes to elab.c, emit.c, and type checker

**Verdict:** Worth considering for v2 after stdlib approach proves limitations. Not suitable for v1.


### Approach E: Data.Seq Style (Clojure-like)

**Design:** Use a protocol-based approach where any type can implement sequence semantics.

```lisp
(defprotocol Seqable
  (seq [self] : (option (Seq a))))

defprotocol Seq
  (first [self] : (option a))
  (next [self] : (option (Seq a))))

;; Vectors are seqable
(extend-protocol Seqable vec
  (seq [v]
    (when (not= 0 (vec/len v))
      (VecSeq v 0))))

;; Custom sequence type
defstruct VecSeq [vec : (vec a) idx :int]

(extend-protocol Seq VecSeq
  (first [s]
    (some (vec/index (:vec s) (:idx s))))
  (next [s]
    (let [next-idx (+ (:idx s) 1)]
      (when (< next-idx (vec/len (:vec s)))
        (some (VecSeq (:vec s) next-idx))))))
```

**Pros:**
- Very flexible — any type can be a sequence
- Integrates well with existing collections
- Familiar to Clojure users

**Cons:**
- **Protocol overhead**: Multiple dispatch is slower
- **More concepts**: Users need to understand protocols, extending types
- **Complexity in stdlib**: More code to maintain
- Typeclass system in Turmeric doesn't support extending existing types easily

**Performance:** O(1) per operation, but with dispatch overhead

**Complexity:** Medium-High

**Verdict:** Good approach but adds unnecessary complexity for v1. Typeclass limitations in current Turmeric make this awkward.


## Decision: Approach A (Iterator Typeclass)

**Winner: Approach A**

| Criteria | A | B | C | D | E |
|---|---|---|---|---|---|
| Performance | ✅ Excellent | ❌ Poor | ⚠️ Limited | ✅ Potential | ⚠️ Good |
| Simplicity | ✅ Simple | ⚠️ Complex | ⚠️ Abstract | ❌ Compiler | ⚠️ Moderate |
| Compatibility | ✅ C99 | ✅ C99 | ✅ C99 | ❌ Compiler | ✅ C99 |
| Borrow checker | ✅ Easy | ⚠️ Hard | ✅ Easy | ❌ Compiler | ⚠️ Moderate |
| Stdlib only | ✅ Yes | ⚠️ Effects | ✅ Yes | ❌ No | ✅ Yes |
| Short-circuit | ✅ Yes | ⚠️ Yes | ❌ No | ✅ Yes | ✅ Yes |
| Infinite seq | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| Chaining | ✅ Efficient | ❌ Inefficient | ⚠️ Limited | ✅ Potential | ✅ Good |

**Rationale:**

1. **Performance**: Approach A provides O(1) iteration with no hidden allocations for chained operations
2. **Simplicity**: Minimal interface (one typeclass, one method) that's easy to understand and implement
3. **Compatibility**: Works entirely in stdlib with no compiler changes
4. **Borrow checker**: Iterator ownership is explicit and trackable
5. **Flexibility**: Can be extended with additional typeclasses (Peekable, Indexed, etc.)
6. **Familiarity**: Matches patterns from Rust, Swift, Kotlin that developers may know
7. **Proven**: Iterator pattern is well-understood and widely used

Approach B (generators) is ruled out by performance concerns — continuation capture is too expensive for sequence iteration. Approach C (Church encoding) is ruled out by the inability to short-circuit. Approach D (built-in syntax) is deferred to v2. Approach E (protocols) adds unnecessary complexity.

---

## Architecture

### Core Types

```lisp
;; Option 1: Minimal Iterator (Recommended for v1)
(defclass Iterator [a]
  (next [self] : (option a)))

;; Option 2: Extended Iterator with peek (v2)
(defclass Peekable [a]
  (peek [self] : (option a))
  (next [self] : (option a)))

;; Sequence: anything that can produce an iterator
(defstruct Seq [iter_fn : (fn [] : (Iterator a))])
```

### Module Structure

```
stdlib/
├── seq/
│   ├── core.tur          ; Iterator, Seq, basic constructors
│   ├── builders.tur      ; range, repeat, cycle, iterate, etc.
│   ├── transform.tur     ; map, filter, take, drop, take-while, etc.
│   ├── combine.tur       ; concat, chain, zip, merge, etc.
│   ├── fold.tur          ; reduce, foldl, foldr, scan, etc.
│   ├── collect.tur       ; into-vec, into-list, to-array, etc.
│   └── predicate.tur     ; any?, all?, find, contains?, etc.
└── seq.tur               ; Main export (re-exports)
```

### File Dependencies

```
seq/core.tur      → typeclass.tur, option.tur
seq/builders.tur  → seq/core.tur
seq/transform.tur → seq/core.tur
seq/combine.tur   → seq/core.tur, seq/transform.tur
seq/fold.tur      → seq/core.tur
seq/collect.tur   → seq/core.tur, vec.tur, list.tur
seq/predicate.tur → seq/core.tur, seq/fold.tur
seq.tur           → seq/*
```

### Compiler Touchpoints

**None for v1.** All implementation is in stdlib. Future optimizations (v2+) may include:
- `src/elab.c`: Detect and fuse adjacent `map` operations
- `src/emit.c`: Special-case emission for common iterator patterns
- `src/types.h`: Add iterator kind for better type error messages

---

## Phases

### Phase LZ0 — Specification and Core Types

**Goal:** Define the Iterator typeclass and basic sequence infrastructure with runnable examples.

**Tasks:**
- [ ] Define `Iterator` typeclass with `next` method
- [ ] Define `Seq` struct with `iter_fn` field
- [ ] Implement `iter-of` for single-element sequences
- [ ] Implement `empty-seq` (always returns `none`)
- [ ] Implement `seq-of` for varargs single-element sequences
- [ ] Implement `seq-from-vec` adapter
- [ ] Implement `seq-from-list` adapter
- [ ] Write fixture tests in `tests/fixtures/seq/`

**Exit Criterion:** Core types defined, basic constructors work, fixtures exist.


### Phase LZ1 — Basic Builders

**Goal:** Implement common sequence constructors.

**Builders to implement:**

| Function | Signature | Description |
|---|---|---|
| `range` | `(-> int int : (Seq int))` | Range from start (exclusive) to end |
| `range-from` | `(-> int int int : (Seq int))` | Range with step |
| `repeat` | `(-> a : (Seq a))` | Infinite repetition of value |
| `repeatedly` | `(-> (fn [] a) : (Seq a))` | Infinite call of thunk |
| `cycle` | `(-> (Seq a) : (Seq a))` | Cycle a finite sequence infinitely |
| `iter` | `(-> a (fn [a] a) : (Seq a))` | Iterate: x, f(x), f(f(x)), ... |
| `constant` | `(-> a : (Seq a))` | Alias for `repeat` |
| `unfold` | `(-> a (fn [a] (option (Tuple a a))) : (Seq a))` | Unfold from seed |

**Tasks:**
- [ ] Implement all builders
- [ ] Add comprehensive fixtures for each
- [ ] Add docstrings for all functions

**Exit Criterion:** All builders implemented and tested.


### Phase LZ2 — Transformations

**Goal:** Implement sequence transformations that chain efficiently.

**Transformations:**

| Function | Signature | Lazy? |
|---|---|---|
| `map` | `(-> (-> a b) (Seq a) : (Seq b))` | ✅ |
| `filter` | `(-> (-> a bool) (Seq a) : (Seq a))` | ✅ |
| `take` | `(-> int (Seq a) : (Seq a))` | ✅ |
| `drop` | `(-> int (Seq a) : (Seq a))` | ✅ |
| `take-while` | `(-> (-> a bool) (Seq a) : (Seq a))` | ✅ |
| `drop-while` | `(-> (-> a bool) (Seq a) : (Seq a))` | ✅ |
| `map-indexed` | `(-> (-> int a b) (Seq a) : (Seq b))` | ✅ |
| `filter-indexed` | `(-> (-> int a bool) (Seq a) : (Seq a))` | ✅ |
| `flat-map` | `(-> (-> a (Seq b)) (Seq a) : (Seq b))` | ✅ |
| `flatten` | `(-> (Seq (Seq a)) : (Seq a))` | ✅ |

**Tasks:**
- [ ] Implement all transformations
- [ ] Verify chaining works: `(->> (range 10) (map inc) (filter even?) (take 5))`
- [ ] Add fixtures for each transformation
- [ ] Add fixtures for chained transformations

**Exit Criterion:** All transformations implemented, chaining verified.


### Phase LZ3 — Combining and Consuming

**Goal:** Implement sequence combination and consumption operations.

**Combinators:**

| Function | Signature |
|---|---|
| `concat` | `(-> (Seq a) (Seq a) : (Seq a))` |
| `chain` | `(-> (Seq (Seq a)) : (Seq a))` |
| `zip` | `(-> (Seq a) (Seq b) : (Seq (Tuple a b)))` |
| `zip-longest` | `(-> a b (Seq a) (Seq b) : (Seq (Tuple a b)))` |
| `interleave` | `(-> (Seq a) (Seq a) : (Seq a))` |

**Consumers:**

| Function | Signature |
|---|---|
| `into-vec` | `(-> (Seq a) : (vec a))` |
| `into-list` | `(-> (Seq a) : int)` | (returns list pointer as int)
| `nth` | `(-> int (Seq a) : (option a))` |
| `first` | `(-> (Seq a) : (option a))` |
| `second` | `(-> (Seq a) : (option a))` |
| `last` | `(-> (Seq a) : (option a))` | (consumes entire seq)
| `count` | `(-> (Seq a) : int)` |
| `reduce` | `(-> (-> a a a) (Seq a) : (option a))` |
| `foldl` | `(-> a (-> a a a) (Seq a) : a)` |
| `foldr` | `(-> a (-> a a a) (Seq a) : a)` |

**Predicate operations:**

| Function | Signature |
|---|---|
| `any?` | `(-> (-> a bool) (Seq a) : bool)` |
| `all?` | `(-> (-> a bool) (Seq a) : bool)` |
| `find` | `(-> (-> a bool) (Seq a) : (option a))` |
| `find-index` | `(-> (-> a bool) (Seq a) : (option int))` |
| `contains?` | `(-> a (Seq a) : bool)` |
| `every?` | `(-> (-> a bool) (Seq a) : bool)` | (short-circuit)

**Tasks:**
- [ ] Implement all combinators
- [ ] Implement all consumers
- [ ] Implement all predicate operations
- [ ] Add fixtures for each

**Exit Criterion:** All operations implemented and tested.


### Phase LZ4 — Advanced Features (v2)

**Deferred to v2:**

- `Peekable` typeclass for lookahead
- `Indexed` typeclass for O(1) random access on supported sequences
- `Reversible` typeclass for bidirectional iteration
- `Cloneable` typeclass for resumable iteration
- `SizeHint` trait for optimization hints
- Custom iterator types (e.g., `ChunkedIterator` for bulk operations)
- Fused operations: `map-filter` → single pass, `flat-map-filter` → single pass
- Compiler-level fusion of adjacent `map` operations
- Specialized iterators for primitive types (int, float) with unboxed values


### Phase LZ5 — Integration (v2)

**Deferred to v2:**

- `Iterable` typeclass for types that can produce sequences (vec, list, str, etc.)
- `IntoIterator` typeclass for ownership-aware iteration
- Lazy sequence support in `for` comprehension syntax (requires compiler changes)
- Range syntax: `(1..10)`, `(1...10)`, `(1..=10)`
- Comprehension syntax: `(for [x xs y ys :when (> x y)] (x y))`


### Phase LZ6 — Performance Optimizations (v3)

**Deferred to v3:**

- Small iterator optimization: Stack-allocate small iterators when possible
- Inlining of iterator methods when types are known
- Iterator fusion in compiler (combine multiple iterator layers into one)
- Specialized codegen for `map` on primitive types
- SIMD-optimized iterators for numeric operations
- Parallel iteration for CPU-bound operations

---

## Implementation Details

### Iterator State Machine

Each iterator maintains its state in a closure. For simple cases, the closure captures local variables:

```lisp
;; range iterator implementation
defn range-iter [start :int end :int] : (Iterator int)
  (let [^mut current start]
    {:next (fn [self]
             (if (= current end)
               none
               (let [result (some current)]
                 (set! current (+ current 1))
                 result)))})
```

This compiles to a C struct with the captured `current` variable and a function pointer for `next`.

### Transformation Chaining

Transformations compose by wrapping iterators:

```lisp
;; map implementation
defn seq-map [f : (-> a b) s : (Seq a)] : (Seq b)
  {:iter_fn (fn []
              (let [inner (iter_fn s)]
                {:next (fn [self]
                         (match (Iterator/next inner)
                           (some x) (some (f x))
                           none none))}))}
```

The key insight: **no intermediate collection is created**. Each `next` call flows through the chain:
1. `map`'s `next` calls `filter`'s `next`
2. `filter`'s `next` calls `range`'s `next`
3. `range`'s `next` produces a value or `none`
4. Each layer transforms the result on the way back

### Memory Management

Iterators follow Turmeric's existing memory model:
- Stack-allocated when possible (closure captures are on stack)
- Heap-allocated when captured by another closure
- Borrow checker ensures no use-after-free
- Reference counting for shared ownership

For sequences that allocate per-element (e.g., `cons` cells), the caller owns the memory and must free it.

### Error Handling

Iterator `next` never throws. Instead:
- `none` signals end of iteration
- Errors during iteration are undefined behavior (match existing `vec/index` semantics)
- Future: could add `Result`-based iteration with `Iterator` returning `Result<option a, E>`

---

## Testing Strategy

### Unit Tests

Each function gets its own fixture file in `tests/fixtures/seq/`. Examples:

```
Tests/fixtures/seq/
├── core.tur              ; Iterator typeclass, basic Seq
├── builders.tur          ; range, repeat, cycle, etc.
├── transform.tur         ; map, filter, take, etc.
├── combine.tur           ; concat, zip, etc.
├── consume.tur           ; into-vec, nth, etc.
├── predicate.tur         ; any?, all?, find, etc.
└── chaining.tur          ; composed transformations
```

### Property Tests

Use existing test infrastructure for property-based testing:

```lisp
;; map preserves length for finite sequences
(defn prop-map-length [s : (Seq int) f : (-> int int)]
  (let [mapped (seq/map f s)
        original (into-vec s)
        result (into-vec mapped)]
    (ensure! (= (vec/len original) (vec/len result)))))

;; filter only includes matching elements
(defn prop-filter-membership [s : (Seq int) p : (-> int bool)]
  (let [filtered (seq/filter p s)]
    (ensure! (all? p filtered))))
```

### Performance Tests

Benchmark iteration speed against hand-written loops:

```lisp
;; Benchmark: sum first N natural numbers
defn bench-seq-sum [n :int]
  (->> (seq/range n)
       (seq/foldl 0 +)))

;; Compare against:
defn bench-loop-sum [n :int]
  (let [^mut sum 0
        ^mut i 0]
    (while (< i n)
      (set! sum (+ sum i))
      (set! i (+ i 1)))
    sum))
```

Target: < 2x overhead vs hand-written loop for simple operations.

---

## Open Questions

1. **Naming**: Should we use `Iterator`/`Seq` or `Iter`/`LazySeq` or `Gen`/`Sequence`?
   - Proposed: `Iterator` and `Seq` (short and clear)

2. **Method naming**: `next` vs `advance` vs `step`?
   - Proposed: `next` (matches Rust, Swift, Kotlin)

3. **Option type**: Should `next` return `option a` or `Result<option a, E>`?
   - v1: `option a` only
   - v2: Add `try-next` for error-aware iteration

4. **Mutability**: Should iterators be mutable (consumed by iteration) or immutable (can be cloned)?
   - v1: Mutable only (simpler)
   - v2: Add `Cloneable` typeclass for resumable iteration

5. **Finite vs infinite**: How to distinguish?
   - v1: No distinction at type level
   - v2: Add `SizeHint` typeclass for optimization

6. **Borrow checker**: How to express "this iterator borrows from that collection"?
   - Use existing borrow annotations: `&Seq` for borrowed, `Seq` for owned

---

## Related Work

| Language | Approach | Notes |
|---|---|---|
| Rust | `Iterator` trait | Most direct inspiration. Our design is simpler (no `Item` associated type in v1) |
| Clojure | `Seqable`, `Sequential` | Protocol-based, very flexible but complex |
| Haskell | Lists, foldable | Lazy by default, but our target is C99 |
| Swift | `Sequence`, `IteratorProtocol` | Similar to our design |
| Kotlin | `Sequence`, `Iterator` | Very similar approach |
| Java | `Stream` | Object-oriented, heap-allocated |
| Python | Generators | Coroutine-based, continuation overhead |

---

## Appendices

### Appendix A: Complete Example

```lisp
(ns example
  (:require [stdlib/seq :as seq]))

;; Create a sequence of squares of even numbers up to 100
defn even-squares []
  (->> (seq/range 0 100)
       (seq/filter (fn [x] (= 0 (mod x 2))))
       (seq/map (fn [x] (* x x)))))

;; Sum them
defn sum-even-squares []
  (seq/foldl 0 + (even-squares)))

;; Get first 10 as a vector
defn first-ten []
  (->> (even-squares)
       (seq/take 10)
       (seq/into-vec)))

;; Check if any square > 5000
defn any-large-squares []
  (seq/any? (fn [x] (> x 5000)) (even-squares)))
```

### Appendix B: Expected C Output

```c
// Simplified: range iterator
typedef struct {
    int64_t current;
    int64_t end;
} RangeIter;

static OptionInt64 range_iter_next(RangeIter* self) {
    if (self->current == self->end) {
        return NONE;
    }
    int64_t result = self->current;
    self->current++;
    return SOME(result);
}

// Map iterator wrapper
typedef struct {
    Iterator* inner;
    int64_t (*func)(int64_t);
} MapIter;

static OptionInt64 map_iter_next(MapIter* self) {
    OptionInt64 inner_result = Iterator_next(self->inner);
    if (inner_result.tag == NONE) {
        return NONE;
    }
    return SOME(self->func(inner_result.value));
}
```

---

## Summary

**Recommendation:** Implement Approach A (Iterator typeclass) in stdlib across phases LZ0–LZ3.

**Next Step:** Begin Phase LZ0 with core type definitions and basic constructors.

**Estimated effort:** ~2-3 weeks for LZ0–LZ3 (stdlib only, no compiler changes).
