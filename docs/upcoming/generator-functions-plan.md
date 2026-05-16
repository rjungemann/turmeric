# Generator Functions Plan for Turmeric

**Status:** Not started. GF0–GF2 planned.

**Prerequisites:** Phase 17 (algebraic effects), Phase 2 (closures).

**Last updated:** 2026-05-14

---

## Summary

This document explores adding generator functions to Turmeric using the existing algebraic effects system. Generator functions allow writing code that appears to produce a sequence of values over time, suspending and resuming execution between yields. The primary question: **what would it take to implement this in Turmeric?**

---

## Motivation

Generator functions provide an ergonomic syntax for producing sequences:

```lisp
;; Desired syntax
defn naturals []
  (while true
    (yield i)
    (set! i (+ i 1))))

;; Usage
defn first-ten []
  (let [gen (naturals)]
    (vec-of (next gen) (next gen) ...)))
```

This is more intuitive than manually constructing iterators for many use cases.

---

## Approach Evaluation

### Approach 1: Algebraic Effects (Recommended)

Use the existing `defeffect` / `perform` / `handle` infrastructure.

```lisp
;; Define Yield effect
defeffect Yield [a] :unit

defn generator [f : (-> :unit)] : (Seq a)
  (let [^mut result (vec-new)]
    (handle (f)
      (Yield [x k] (vec-push! result x)
                    (resume k))
      (Return [_] result))
    (seq/from-vec result)))

;; Usage
defn range-gen [start :int end :int]
  (generator
    (fn []
      (let [^mut i start]
        (while (< i end)
          (perform (Yield i))
          (set! i (+ i 1)))))))
```

**Pros:**
- Reuses existing, tested infrastructure
- No compiler changes needed
- Can implement today in stdlib
- Familiar to users of Python/JS generators

**Cons:**
- Continuation capture is expensive (O(n) time/space per yield)
- Not suitable for performance-critical iteration
- Each `yield` captures entire stack frame

**Work estimate:** ~1-2 days for basic implementation


### Approach 2: State Machine Compilation (Compiler Support)

Compiler transforms generator functions into explicit state machines.

```lisp
;; Surface syntax
defn range-gen [start :int end :int]
  (gen []
    (let [^mut i start]
      (while (< i end)
        (yield i)
        (set! i (+ i 1))))))

;; Compiler desugars to state machine with resume points
```

**Pros:**
- Zero overhead compared to hand-written iterators
- Can be extremely efficient
- State machine is predictable and debuggable

**Cons:**
- Requires significant compiler changes (elab.c, emit.c)
- Complex state machine generation
- Harder to implement correctly
- Must handle arbitrary control flow (loops, conditionals, early returns)

**Work estimate:** ~3-4 weeks for full implementation


### Approach 3: Iterator-Based (No Effects)

Define a generator as a function that returns an iterator.

```lisp
(defalias Generator<a> (-> : (Iterator a)))

defn make-generator [f : (-> (-> a :unit) :unit)] : (Generator a)
  (fn []
    (let [^mut next-val none]
      {:next (fn [self]
               (if (some? next-val)
                 (let [result next-val]
                   (set! next-val none)
                   result)
                 (let [result (atomically
                                (let [val (atom none)]
                                  (f (fn [x] (reset! val (some x))))
                                  @val))]
                   (set! next-val result)
                   (if (some? result) result none))))}))
```

**Pros:**
- No effects needed
- Can be efficient if callbacks are inlined
- Compatible with existing Seq design

**Cons:**
- Awkward syntax — must pass callback
- Hard to express naturally
- Still has some overhead

**Work estimate:** ~2-3 days


---

## Decision Matrix

| Criteria | Effects | State Machine | Iterator-based |
|---|---|---|---|
| Implementation effort | ✅ **1-2 days** | ❌ 3-4 weeks | ⚠️ 2-3 days |
| Performance | ❌ O(n) per yield | ✅ O(1) | ⚠️ O(1)-ish |
| Compiler changes | ✅ None | ❌ Significant | ✅ None |
| Syntax quality | ✅ Natural | ✅ Natural | ❌ Awkward |
| Maintainability | ✅ Low | ❌ High | ⚠️ Medium |
| Risk | ✅ Low | ⚠️ Medium | ✅ Low |

---

## Recommended Approach: Effects-Based (Approach 1)

**Choice:** Approach 1 (Algebraic Effects) for v1.

**Rationale:**
- **Minimal work**: Can be implemented in stdlib with no compiler changes
- **Proven infrastructure**: Effects system is stable and tested
- **Low risk**: If it's too slow, users can use explicit iterators instead
- **Fast to prototype**: 1-2 days to get working implementation
- **Gateway to Approach 2**: Can later add compiler optimizations for common patterns

**Trade-off accepted:** Performance will be poor for high-volume iteration, but this is acceptable for:
- Prototyping
- Infrequent iteration
- Non-performance-critical code
- Teaching/learning

---

## Work Breakdown

### Phase GF0 — Design & Feasibility (1 day)

**Tasks:**
- [ ] Verify `defeffect` can carry values
- [ ] Test continuation capture performance
- [ ] Write proof-of-concept `Yield` effect
- [ ] Benchmark continuation capture cost
- [ ] Document limitations

**Deliverable:** Working proof-of-concept, performance baseline.


### Phase GF1 — Core Implementation (1 day)

**Tasks:**
- [ ] Define `Yield` effect typeclass
- [ ] Implement `Generator` type (wraps effectful function)
- [ ] Implement `next` function for generators
- [ ] Implement `collect` to materialize into vec
- [ ] Implement `gen->seq` adapter to Seq type
- [ ] Write basic fixtures

**Code location:** `stdlib/gen.tur` (~200 lines)

**Deliverable:** Working generator functions, basic tests.


### Phase GF2 — Ergonomic API (1 day)

**Tasks:**
- [ ] Define `gen` macro for cleaner syntax
- [ ] Implement `yield*` for yielding multiple values
- [ ] Implement `return` for early termination
- [ ] Add `gen-map`, `gen-filter` helpers
- [ ] Add comprehensive docstrings
- [ ] Write advanced fixtures

**Deliverable:** Ergonomic API, full test coverage.


### Total v1 Estimate: **3 days**

---

## Detailed Implementation

### Phase GF0: Proof of Concept

```lisp
;; Verify effects can carry values
defeffect Yield [a] :unit

defn test-yield []
  (let [result (ref (vec-new))]
    (handle
      (do
        (perform (Yield 1))
        (perform (Yield 2))
        (perform (Yield 3)))
      (Yield [x k]
        (vec-push! (deref result) x)
        (resume k))))
  (deref result))  ; => [1, 2, 3]
```

**Expected outcome:** This should work with existing effects infrastructure.

### Phase GF1: Core Types

```lisp
;; Generator: a suspended computation that can yield values
(defstruct Generator [thunk : (fn [] :unit)])

;; Create a generator from an effectful function
defn make-generator [f : (-> :unit)] : (Generator a)
  (Generator f)

;; Get the next value from a generator
defn gen-next [g : (Generator a)] : (option a)
  (let [result (ref none)]
    (set! (:thunk g)
      (handle
        ((:thunk g))
        (Yield [x k]
          (reset! result (some x))
          (resume k))
        (Return [_] (reset! result none))))
    @result)

;; Collect all values from a generator into a vector
defn gen-collect [g : (Generator a)] : (vec a)
  (let [vec (vec-new)]
    (while (some? (let [v (gen-next g)]
                    (when (some? v) (vec-push! vec (some v)))
                    v)))
    vec)
```

### Phase GF2: Ergonomic Macros

```lisp
;; gen macro: wraps function body with generator creation
defmacro gen [params & body]
  (let [f-name (gensym "gen-fn")]
    `(fn ~params
       (make-generator (fn [] ~@body))))

;; yield macro: performs Yield effect
defmacro yield [value]
  `(perform (Yield ~value))

;; Usage
defn range-gen [start :int end :int]
  (gen []
    (let [^mut i start]
      (while (< i end)
        (yield i)
        (set! i (+ i 1))))))
```

---

## Performance Analysis

### Benchmark: Sum First N Numbers

```lisp
;; Generator version
defn sum-gen [n :int]
  (let [g (gen []
            (let [^mut i 0]
              (while (< i n)
                (yield i)
                (set! i (+ i 1)))))]
    (let [^mut sum 0]
      (while (some? (let [v (gen-next g)]
                      (when (some? v) (set! sum (+ sum (some v))))
                      v)))
      sum))

;; Hand-written loop
defn sum-loop [n :int]
  (let [^mut sum 0
        ^mut i 0]
    (while (< i n)
      (set! sum (+ sum i))
      (set! i (+ i 1)))
    sum)
```

**Expected results:**
- N = 10: ~10-100x slower (continuation overhead dominates)
- N = 1000: ~5-50x slower
- N = 10000: ~2-20x slower

**Verdict:** Effects-based generators are **not suitable** for performance-critical iteration, but are **fine** for:
- Prototyping
- Small datasets
- Infrequent operations
- Code clarity where performance isn't critical

---

## Limitations

### Inherent to Effects-Based Approach:

1. **No resume after completion**: Once a generator finishes, it cannot be restarted
2. **No cloneable generators**: Cannot have two independent iterators over same generator
3. **Stack growth**: Deeply nested generators may cause stack overflow
4. **Memory overhead**: Each yield captures entire continuation (stack frame + heap allocations)
5. **No tail-call optimization**: Continuations prevent TCO through yield points

### Can Be Added Later:

1. **State machine compilation** (Phase GF3): Compiler transforms `gen` blocks into explicit state machines
2. **Resume support**: Allow restarting a generator
3. **Clone support**: Allow multiple independent iterators
4. **Yield from**: Delegate to another generator

---

## Comparison with Lazy Sequences

| Feature | Generators (Effects) | Lazy Sequences (Iterator) |
|---|---|---|
| Performance | ❌ O(n) per yield | ✅ O(1) per step |
| Implementation | ✅ 3 days, stdlib | ✅ 2-3 weeks, stdlib |
| Syntax | ✅ Natural | ⚠️ Functional |
| Infinite seq | ✅ Yes | ✅ Yes |
| Chaining | ❌ Awkward | ✅ Natural |
| Compile to C | ✅ Yes | ✅ Yes |
| Memory | ❌ High per yield | ✅ Low per step |
| Borrow checker | ⚠️ Complex | ✅ Simple |

**Conclusion:** Generator functions and lazy sequences solve **different problems**:
- Generators: Natural syntax for **producing** sequences
- Lazy sequences: Efficient **consumption** and **transformation** of sequences

**Recommendation:** Implement both. Generators for ergonomic production, lazy sequences for efficient consumption.

---

## Phases Summary

| Phase | Duration | Deliverables |
|---|---|---|
| GF0 | 1 day | Proof of concept, performance baseline |
| GF1 | 1 day | Core types, basic API, tests |
| GF2 | 1 day | Ergonomic macros, full API, docs |
| **v1 Total** | **3 days** | Working generator functions |
| GF3 | 2-3 weeks | Compiler-based state machine (optional v2) |

---

## Open Questions

1. **Naming**: `Generator` vs `Gen` vs `Producer`?
2. **Effect name**: `Yield` vs `Produce` vs `Emit`?
3. **Multiple yield types**: Can a single generator yield different types? (Probably not in v1)
4. **Early return**: How to signal completion early? (Could use `Return` effect)
5. **Exception handling**: What happens if an exception is thrown during yield?

---

## Related Work

| Language | Implementation | Notes |
|---|---|---|
| Python | Native generators | Coroutine-based, efficient |
| JavaScript | Generator functions | ES6, yields {value, done} |
| Rust | Generators (unstable) | State machine based |
| C# | `yield return` | Compiler-generated state machine |
| Go | Channels | Not generators, but similar use cases |
| Haskell | Lazy lists | Purely functional, no side effects |

---

## Summary

**Bottom line:** Generator functions can be implemented in **~3 days** using Turmeric's existing algebraic effects system. The implementation would have **significant performance overhead** (continuation capture per yield) but would provide a **natural, ergonomic syntax** for producing sequences.

**Recommendation:** 
- Implement effects-based version (Approach 1) for v1 — low cost, quick delivery
- Defer state machine compilation (Approach 2) to v2 if performance becomes an issue
- Consider both generators (production) and lazy sequences (consumption) as complementary features

**Next step:** If you want to proceed, start with Phase GF0 (proof of concept) to verify the approach works with existing effects infrastructure.
