# Generators and Lazy Sequences Plan

**Status:** Not started. GF0-GF2, LZ0-LZ4 planned.

**Prerequisites:** Phase 2 (closures), Phase 15 (typeclasses).

**Last updated:** 2026-05-18

---

## Summary

This plan covers two related features:

1. **Generator functions** (GF phases): The compiler transforms `gen` blocks with
   `yield` into explicit C state machines. This produces zero-overhead, resumable
   iterators with no continuation capture and no heap allocation per yield.

2. **Lazy sequences** (LZ phases): A sequence library built on top of generators.
   Because generators compile to state machines rather than capturing continuations,
   they are efficient enough to serve as the foundation for all lazy sequence
   operations.

The LZ phases depend on all GF phases being complete.

---

## Approach

### Generators: State Machine Compilation

The compiler desugars `gen` blocks into structs that carry resume-point tags and
captured-variable fields. Each `yield` becomes a save-state/return; each call to
`next` dispatches on the current state tag and resumes from the saved point.

```lisp
;; Surface syntax
defn range-gen [start :int end :int] : (Generator :int)
  (gen []
    (let [^mut i start]
      (while (< i end)
        (yield i)
        (set! i (+ i 1)))))

;; Compiler emits roughly:
;;
;; typedef struct { int state; int i; int end; } RangeGen;
;; static Option_int range_gen_next(RangeGen* g) {
;;   switch (g->state) {
;;     case 0: g->i = start; g->end = end;  /* fall through */
;;     case 1: if (g->i >= g->end) { g->state = -1; return NONE; }
;;             { int v = g->i; g->i++; g->state = 1; return SOME(v); }
;;     default: return NONE;
;;   }
;; }
```

This compiles to idiomatic C99 with no dynamic allocation and no function pointer
indirection per yield.

### Lazy Sequences: Generator-Based

Lazy sequences are implemented entirely in stdlib as generator functions. Each
combinator (`map`, `filter`, `take`, ...) is a `gen` that drives an inner
generator. Because generators are zero-cost, chained combinators compile to
equivalent nested switch-dispatch with no intermediate allocations.

```lisp
;; seq/map wraps an inner generator
defn seq-map [f g]
  (gen []
    (while (some? (let [v (gen-next g)]
                    (when (some? v) (yield (f (unwrap v))))
                    v))))
```

---

## Phase GF0 -- Generator State Machine Design (1 week)

**Goal:** Fully specify the IR and C emission strategy before touching the compiler.

**Tasks:**
- [ ] Define the `Generator` struct representation: state tag + captured-variable
      fields per `gen` body
- [ ] Enumerate control-flow forms that cross yield points: `while`, `if`,
      `cond`, `do`, `let`, nested `gen`
- [ ] Specify how variables live across yield points (captured in struct vs.
      local C variables)
- [ ] Write representative hand-compiled C output for: simple loop, nested loops,
      early return via `return`, conditional yield
- [ ] Define `(Generator a)` surface type and its `next`/`done` operations
- [ ] Decide: statically sized struct per `gen` vs. arena-allocated dynamic struct
- [ ] Document limitations: no `gen` inside `match` arms that span a yield (v1),
      no recursive generators (v1)

**Deliverable:** Design document with hand-compiled C examples and a list of
supported/unsupported control-flow patterns.


## Phase GF1 -- Compiler Implementation (2-3 weeks)

**Goal:** End-to-end compilation of `gen`/`yield` forms to C state machines.

**Affected files:**
- `src/compiler/elab_forms.c` -- elaborate `gen` and `yield` forms; infer
  generator element type from yield expressions
- `src/compiler/elab_core.c` -- unify `(Generator a)` type
- `src/compiler/types.h` / `types.c` -- add `TY_GENERATOR` kind, store element
  type and captured-variable list
- `src/compiler/emit_stmt.c` -- emit state-machine switch dispatch for `gen` body
- `src/compiler/emit_expr.c` -- emit `yield` as assign-state/return-SOME; emit
  `gen-next` as call into the state machine
- `src/compiler/emit_module.c` -- emit the `typedef struct` for each `gen` closure

**Tasks:**
- [ ] Add `TY_GENERATOR` to `types.h`; store element type and captured variable
      list per generator instance
- [ ] Elaborate `(gen [...] body)`: collect all variables live across yield
      points; build generator type
- [ ] Elaborate `(yield expr)`: check expr type matches generator element type;
      record yield point
- [ ] Emit generator struct typedef (state tag + captured fields)
- [ ] Emit `_next` function: switch on state tag, execute body segments between
      yields
- [ ] Emit `(gen-next g)` call-site as invocation of `_next`
- [ ] Handle `while` loops that contain yields (loop-back state tag)
- [ ] Handle `if`/`cond` branches that contain yields (branch state tags)
- [ ] Handle `let`-bound variables that span a yield (promote to struct field)
- [ ] Add `(gen-done? g)` predicate (state == -1)
- [ ] Write fixture: simple loop generator (`range-gen`)
- [ ] Write fixture: generator with `if` branch
- [ ] Write fixture: generator with early return
- [ ] Write fixture: nested generator (outer drives inner)

**Exit criterion:** All fixtures pass; generated C compiles and runs correctly
under Valgrind with no leaks.


## Phase GF2 -- Generator Standard Library (3 days)

**Goal:** Ergonomic surface API and helper macros for generators.

**Code location:** `stdlib/gen.tur`

**Tasks:**
- [ ] Define `(Generator a)` as an opaque struct wrapper
- [ ] Define `gen-next : (Generator a) -> (option a)`
- [ ] Define `gen-done? : (Generator a) -> bool`
- [ ] Define `gen-collect : (Generator a) -> (vec a)` (materialise all values)
- [ ] Define `gen-for-each : (fn [a] :unit) (Generator a) -> :unit`
- [ ] Define `gen-nth : int (Generator a) -> (option a)` (consume to nth)
- [ ] Define `yield*` macro: yield all values from an inner generator
- [ ] Add `;;;` docstrings for all exported functions
- [ ] Write fixtures for `gen-collect`, `gen-for-each`, `gen-nth`, `yield*`

**Deliverable:** `stdlib/gen.tur`, fixtures passing.

---

## Phase LZ0 -- Core Sequence Types (3 days)

**Goal:** Define `Seq` as a thin wrapper around a generator-producing thunk.

**Code location:** `stdlib/seq/core.tur`

**Tasks:**
- [ ] Define `(Seq a)` as `(defstruct Seq [mk : (fn [] (Generator a))])`
- [ ] Define `seq-iter : (Seq a) -> (Generator a)` (call `mk` to get a fresh
      generator)
- [ ] Define `seq-of : a -> (Seq a)` (single-element sequence)
- [ ] Define `empty-seq : (Seq a)` (always done)
- [ ] Define `seq-from-vec : (vec a) -> (Seq a)`
- [ ] Define `seq-from-list : list -> (Seq a)`
- [ ] Write fixtures in `tests/fixtures/seq/core/`

**Exit criterion:** Core types and adapters work; fixtures pass.


## Phase LZ1 -- Sequence Builders (3 days)

**Goal:** Implement standard constructors for common infinite and finite sequences.

**Code location:** `stdlib/seq/builders.tur`

**Tasks:**

| Function | Signature | Description |
|---|---|---|
| `seq/range` | `int int -> (Seq int)` | Half-open range [start, end) |
| `seq/range-step` | `int int int -> (Seq int)` | Range with step |
| `seq/repeat` | `a -> (Seq a)` | Infinite repetition |
| `seq/repeatedly` | `(fn [] a) -> (Seq a)` | Infinite thunk calls |
| `seq/cycle` | `(Seq a) -> (Seq a)` | Cycle finite seq infinitely |
| `seq/iterate` | `a (fn [a] a) -> (Seq a)` | x, f(x), f(f(x)), ... |
| `seq/unfold` | `a (fn [a] (option (pair a a))) -> (Seq a)` | Unfold from seed |

- [ ] Implement all builders as `gen`-based generators
- [ ] Add `;;;` docstrings for each
- [ ] Write fixtures in `tests/fixtures/seq/builders/`

**Exit criterion:** All builders produce correct output; infinite sequences
truncated via `seq/take` work correctly.


## Phase LZ2 -- Transformations (1 week)

**Goal:** Lazy combinators that chain without intermediate allocation.

**Code location:** `stdlib/seq/transform.tur`

**Tasks:**

| Function | Signature | Notes |
|---|---|---|
| `seq/map` | `(fn [a] b) (Seq a) -> (Seq b)` | |
| `seq/filter` | `(fn [a] bool) (Seq a) -> (Seq a)` | |
| `seq/take` | `int (Seq a) -> (Seq a)` | |
| `seq/drop` | `int (Seq a) -> (Seq a)` | |
| `seq/take-while` | `(fn [a] bool) (Seq a) -> (Seq a)` | |
| `seq/drop-while` | `(fn [a] bool) (Seq a) -> (Seq a)` | |
| `seq/map-indexed` | `(fn [int a] b) (Seq a) -> (Seq b)` | |
| `seq/filter-map` | `(fn [a] (option b)) (Seq a) -> (Seq b)` | map + filter in one pass |
| `seq/flat-map` | `(fn [a] (Seq b)) (Seq a) -> (Seq b)` | |
| `seq/flatten` | `(Seq (Seq a)) -> (Seq a)` | |

- [ ] Implement all transformations as `gen`-based wrappers
- [ ] Verify chaining: `(->> (seq/range 0 100) (seq/filter even?) (seq/map sq) (seq/take 5))`
- [ ] Add `;;;` docstrings
- [ ] Write fixtures for each; write a fixture for a 3-deep chain
- [ ] Write a fixture comparing chained seq output against a hand-written loop

**Exit criterion:** All transformations work; chained pipeline produces correct
results with no intermediate vecs.


## Phase LZ3 -- Combinators and Consumers (1 week)

**Goal:** Sequence combination and terminal operations.

**Code locations:** `stdlib/seq/combine.tur`, `stdlib/seq/consume.tur`

### Combinators

| Function | Signature |
|---|---|
| `seq/concat` | `(Seq a) (Seq a) -> (Seq a)` |
| `seq/chain` | `(Seq (Seq a)) -> (Seq a)` |
| `seq/zip` | `(Seq a) (Seq b) -> (Seq (pair a b))` |
| `seq/zip-with` | `(fn [a b] c) (Seq a) (Seq b) -> (Seq c)` |
| `seq/interleave` | `(Seq a) (Seq a) -> (Seq a)` |

### Consumers

| Function | Signature |
|---|---|
| `seq/into-vec` | `(Seq a) -> (vec a)` |
| `seq/into-list` | `(Seq a) -> list` |
| `seq/nth` | `int (Seq a) -> (option a)` |
| `seq/first` | `(Seq a) -> (option a)` |
| `seq/last` | `(Seq a) -> (option a)` |
| `seq/count` | `(Seq a) -> int` |
| `seq/reduce` | `(fn [a a] a) (Seq a) -> (option a)` |
| `seq/foldl` | `b (fn [b a] b) (Seq a) -> b` |
| `seq/for-each` | `(fn [a] :unit) (Seq a) -> :unit` |
| `seq/any?` | `(fn [a] bool) (Seq a) -> bool` |
| `seq/all?` | `(fn [a] bool) (Seq a) -> bool` |
| `seq/find` | `(fn [a] bool) (Seq a) -> (option a)` |
| `seq/find-index` | `(fn [a] bool) (Seq a) -> (option int)` |

- [ ] Implement all combinators and consumers
- [ ] Verify `any?` and `all?` short-circuit correctly
- [ ] Add `;;;` docstrings
- [ ] Write fixtures for each operation
- [ ] Write a fixture for the complete pipeline:
      `(->> (seq/range 0 1000) (seq/filter even?) (seq/map sq) (seq/take-while (fn [x] (< x 10000))) (seq/foldl 0 +))`

**Exit criterion:** All operations correct; `any?`/`all?` short-circuit verified
via a side-effecting fixture; full pipeline fixture passes.

## Phase LZ4 -- Range Types (1 week)

**Goal:** A first-class `Range` type modelled on Rebellion's range semantics. Ranges
represent continuous intervals of ordered values with inclusive, exclusive, or
unbounded endpoints. They are a value type, not a sequence -- but can be converted
to a `Seq` for discrete element types.

**Code location:** `stdlib/range.tur`

### Core Types

```lisp
;; A bound is one of three cases
(defstruct InclusiveBound [endpoint :a])
(defstruct ExclusiveBound [endpoint :a])
;; unbounded is a sentinel (nil-value / none used at call sites)

;; A range pairs a lower and upper bound; each is (option Bound)
(defstruct Range
  [lower : (option (RangeBound a))]   ; none = unbounded below
  [upper : (option (RangeBound a))]) ; none = unbounded above
```

The comparator for ordering is supplied by the `Ord` typeclass on the element
type `a`, rather than being embedded in the range value (Turmeric has no
first-class comparator objects).

### Constructors

| Function | Interval | Description |
|---|---|---|
| `closed-range lo hi` | [lo, hi] | Both inclusive |
| `open-range lo hi` | (lo, hi) | Both exclusive |
| `closed-open-range lo hi` | [lo, hi) | Inclusive lower, exclusive upper |
| `open-closed-range lo hi` | (lo, hi] | Exclusive lower, inclusive upper |
| `at-least-range lo` | [lo, +inf) | Inclusive lower, unbounded above |
| `greater-than-range lo` | (lo, +inf) | Exclusive lower, unbounded above |
| `at-most-range hi` | (-inf, hi] | Unbounded below, inclusive upper |
| `less-than-range hi` | (-inf, hi) | Unbounded below, exclusive upper |
| `singleton-range v` | [v, v] | Exactly one value |
| `unbounded-range` | (-inf, +inf) | No bounds |

### Predicates

| Function | Description |
|---|---|
| `range-contains? r v` | True if v falls within r |
| `range-encloses? r other` | True if r contains every value in other |
| `range-connected? r1 r2` | True if no gap exists between r1 and r2 (touching is ok) |
| `range-overlaps? r1 r2` | True if r1 and r2 share a nonempty intersection |
| `bounded-range? r` | Both bounds present |
| `bounded-above? r` | Has upper bound |
| `bounded-below? r` | Has lower bound |
| `unbounded-above? r` | No upper bound |
| `unbounded-below? r` | No lower bound |
| `singleton-range? r` | Contains exactly one value |
| `empty-range? r` | Contains no values (e.g. open endpoints at equal values) |
| `nonempty-range? r` | Contains at least one value |

### Set Operations

| Function | Description |
|---|---|
| `range-span r1 r2` | Smallest range enclosing both (convex hull) |
| `range-gap r1 r2` | Largest range between two non-overlapping ranges |
| `range-intersection r1 r2` | Largest range enclosed by both (requires connected) |

### Sequence Integration

```lisp
;; Convert a bounded integer range to a lazy Seq
;; Works for closed-range, closed-open-range, open-closed-range, open-range
;; Requires Ord + discrete step; step defaults to 1
seq/from-range : (Range int) -> (Seq int)
seq/from-range-step : int (Range int) -> (Seq int)
```

**Tasks:**
- [ ] Define `RangeBound` variants and `Range` struct
- [ ] Implement all constructors with validation (lower <= upper; if equal, at least one inclusive)
- [ ] Implement all predicates
- [ ] Implement `range-span`, `range-gap`, `range-intersection`
- [ ] Implement `seq/from-range` and `seq/from-range-step` for integer ranges
- [ ] Add `;;;` docstrings for all exported functions
- [ ] Write fixtures in `tests/fixtures/range/`:
  - Construction and validation
  - `range-contains?` for each bound combination
  - `range-overlaps?` vs `range-connected?` distinction
  - `range-span`, `range-gap`, `range-intersection`
  - `seq/from-range` produces correct elements

**Exit criterion:** All fixtures pass; empty-range and singleton-range edge cases
correct; `range-gap` on overlapping ranges signals an error.

---

## Deferred (v2+)

### `Peekable` typeclass

Extends `Generator` with the ability to inspect the next element without consuming
it. Requires buffering one element internally.

```lisp
(defclass Peekable [^g]
  (peek [self] : (option a)))
```

Useful for parsers, tokenisers, and any algorithm that needs one-element lookahead.
Implement as a wrapper generator that buffers the last `gen-next` result.

### `Reversible` typeclass

Bidirectional iteration: generators that can step both forward and backward. Only
meaningful for sequences backed by indexable storage (vec, array). Requires the
underlying generator to support a `prev` operation.

```lisp
(defclass Reversible [^g]
  (prev [self] : (option a)))
```

Deferred because most use cases are covered by materialising to `vec` and indexing.

### `SizeHint` typeclass

Allows a generator to advertise a lower and optional upper bound on its remaining
element count. Enables pre-allocation optimisations in consumers like
`seq/into-vec`.

```lisp
(defclass SizeHint [^g]
  (size-hint [self] : (pair int (option int))))  ; (lower, upper?)
```

`range` generators would return exact hints; `filter` generators would return
`(0, upper?)` since filtering may discard elements.

### `seq/scan`

Generalised prefix-sum: like `foldl`, but yields every intermediate accumulator
state rather than only the final result.

```lisp
seq/scan : b (fn [b a] b) (Seq a) -> (Seq b)

;; Example: running sum over [1 2 3 4] with init 0
;; => (Seq 1 3 6 10)
```

Useful for running totals, cumulative statistics, and observing state evolution.

### `seq/group-by` and `seq/partition`

`seq/group-by` collects elements into a map keyed by the result of a classifier
function. Strict (must consume the full sequence to return).

```lisp
seq/group-by : (fn [a] k) (Seq a) -> (map k (vec a))
```

`seq/partition` splits a sequence into two vecs -- elements that satisfy a
predicate and elements that do not. Also strict.

```lisp
seq/partition : (fn [a] bool) (Seq a) -> (pair (vec a) (vec a))
```

Both are deferred because they require a working `map` type (HAMT).

### Compiler-level `map` fusion

When the elaborator sees `(seq/map f (seq/map g xs))`, rewrite it to
`(seq/map (fn [x] (f (g x))) xs)` before type-checking completes. One generator,
one state-machine dispatch per element instead of two.

**Affected file:** `src/compiler/elab_forms.c` -- pattern-match on `(seq/map _ (seq/map _ _))` in the form elaboration pass and substitute the composed call.

Safe only when `f` and `g` are provably pure (no observable side effects). v1
restriction: only fuse when both arguments are literal `fn` expressions or named
functions known to be pure.

### `for` comprehension over sequences

The existing `for` macro already desugars to `.bind`/`.pure`/`.empty`. Making it
work with `Seq` requires only a `Monad [Seq]` instance in stdlib (`.bind` =
`seq/flat-map`, `.pure` = `seq-of`, `.empty` = `empty-seq`). This is stdlib work,
not a compiler change.

A future dedicated comprehension syntax -- with static fusion, guard optimisation,
and better error messages -- would require compiler changes, and is deferred to v2.

### Specialised numeric iterators

Unboxed primitive iterators that avoid the `int64_t` boxing overhead for
performance-critical numeric pipelines. Requires compiler support for
monomorphised generator types.

---

## Phase Summary

| Phase | Est. Duration | Deliverables |
|---|---|---|
| GF0 | 1 week | State machine design doc, hand-compiled C examples |
| GF1 | 2-3 weeks | Compiler support for `gen`/`yield`, fixtures |
| GF2 | 3 days | `stdlib/gen.tur`, helper macros, fixtures |
| LZ0 | 3 days | `stdlib/seq/core.tur`, adapters, fixtures |
| LZ1 | 3 days | `stdlib/seq/builders.tur`, fixtures |
| LZ2 | 1 week | `stdlib/seq/transform.tur`, chaining fixtures |
| LZ3 | 1 week | `stdlib/seq/combine.tur`, `consume.tur`, fixtures |
| LZ4 | 1 week | `stdlib/range.tur`, Range type, `seq/from-range`, fixtures |
| **Total** | **~8-9 weeks** | Working generators + full sequence library + Range types |
