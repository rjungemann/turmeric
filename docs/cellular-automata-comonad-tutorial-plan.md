# Cellular Automata with Comonads — Tutorial & Example Plan for Turmeric

> **Status:** Complete  
> **Prerequisite:** HKT Phase H3 (Functor, Applicative, Monad typeclasses) complete  
> **Related:** [higher-kinded-types-plan.md](../archive/higher-kinded-types-plan.md)

**Deliverables shipped:**
- `stdlib/comonad.tur` — Comonad typeclass + Identity + Pair instances ✓
- `stdlib/zipper.tur` — 1D Zipper comonad for 1D cellular automata ✓
- `stdlib/grid.tur` — GridCtx comonad for 2D cellular automata ✓
- `examples/cellular-automata.tur` — Conway's Game of Life (imperative + comonadic) ✓
- `docs/guides/cellular-automata-comonad-tutorial.md` — Step-by-step tutorial ✓
- Test fixtures: `grid-basic`, `grid-comonad`, `game-of-life-blinker`, `game-of-life-block`,
  `zipper-comonad`, `comonad-identity`, `flat-array-access` ✓

---

## Executive Summary

This document outlines a **tutorial and example implementation** of Conway's Game of Life (and other cellular automata) using **comonads** in Turmeric. Comonads provide an elegant, declarative way to express cellular automata rules by treating the grid as a comonadic context where each cell can "see" its neighborhood.

**Why comonads for cellular automata?**

| Concept | Comonadic Interpretation |
|---|---|
| Grid | A comonadic structure `w a` where `w` is the grid type |
| Current cell value | `extract wa` — get the value at a position |
| Neighborhood access | `duplicate wa` — get the entire grid from each cell's perspective |
| Rule application | `extend f` — apply a rule function to every cell with its neighborhood |
| Next generation | `extend rule >>= extract` — compute next state for all cells |

**Deliverables:**
1. `stdlib/comonad.tur` — Comonad typeclass and standard comonads
2. `stdlib/grid.tur` — Grid data structure with Comonad instance
3. `examples/cellular-automata.tur` — Conway's Game of Life implementation
4. `docs/guides/cellular-automata-comonad-tutorial.md` — Step-by-step tutorial

---

## Table of Contents

1. [Background: Comonads](#1-background-comonads)
2. [Comonad Typeclass Design](#2-comonad-typeclass-design)
3. [Grid as a Comonad](#3-grid-as-a-comonad)
4. [Conway's Game of Life Implementation](#4-conways-game-of-life-implementation)
5. [Tutorial Structure](#5-tutorial-structure)
6. [Implementation Phases](#6-implementation-phases)
7. [Testing Strategy](#7-testing-strategy)
8. [Performance Considerations](#8-performance-considerations)
9. [Extensions](#9-extensions)

---

## 1. Background: Comonads

### What is a Comonad?

A **comonad** is the categorical dual of a monad. Where monads provide a way to sequence computations with effects, comonads provide a way to consume contextual data.

**Monad** (sequencing computations):
```haskell
class Monad m where
  return :: a -> m a
  bind   :: m a -> (a -> m b) -> m b
```

**Comonad** (consuming context):
```haskell
class Comonad w where
  extract :: w a -> a
  extend :: (w a -> b) -> w a -> w b  -- or: duplicate :: w a -> w (w a)
```

### Intuition

Think of a comonad as a **data structure with a current position**:
- `extract` — get the value at the current position
- `duplicate` — get a copy of the structure with every possible position as the current position
- `extend f` — apply `f` to each possible view, building a new structure

### Why Cellular Automata?

Cellular automata are the **killer app** for comonads because:

1. **Local rules, global behavior** — Each cell's next state depends only on its local neighborhood
2. **Uniform structure** — The grid provides a regular, comonadic context
3. **Declarative rules** — Rules are expressed as pure functions from neighborhood to state
4. **Elegant composition** — Complex automata can be built by composing simple comonadic operations

### Mathematical Connection

A cellular automaton can be seen as a **coalgebra** for the comonad. The transition function:
```
δ :: Grid a -> Grid (Grid a)  -- duplicate: every cell sees the whole grid
f :: Grid a -> a             -- rule: compute next state from neighborhood

next :: Grid a -> Grid a
next = fmap f . δ           -- apply rule to every cell's view
     = extend f             -- same as comonadic extend
```

---

## 2. Comonad Typeclass Design

### Typeclass Definition

Assuming HKT Phase H3 is complete with `Functor` and other typeclasses:

```clojure
;; stdlib/comonad.tur

;; Comonad typeclass — the dual of Monad
(defclass Comonad [w : * -> *] : Functor w
  ;; Get the current position's value
  (extract [a : *] [wa : w a] : a)
  
  ;; One of: duplicate or extend (extend can be derived from duplicate + fmap)
  ;; Option A: Primitive duplicate
  (duplicate [a : *] [wa : w a] : (w (w a)))
  
  ;; Option B: Primitive extend (more efficient for some comonads)
  ;; (extend [a : *, b : *] [wa : w a, f : (-> (w a) b)] : (w b)))
```

**Recommendation:** Use `extend` as primitive (Option B). Many comonads have efficient `extend` implementations that don't require materializing the full `duplicate`d structure.

```clojure
(defclass Comonad [w : * -> *] : Functor w
  (extract [a : *] [wa : w a] : a)
  (extend [a : *, b : *] [wa : w a, f : (-> (w a) b)] : (w b)))
```

### Derived Operations

```clojure
;; duplicate can be derived from extend
(defn duplicate [^Comonad w, a : *] [wa : w a] : (w (w a))
  (extend wa identity))

;; Co-Kleisli composition (dual of Kleisli composition for monads)
(defn co-kleisli [^Comonad w, a : *, b : *, c : *]
  [f : (-> (w a) b), g : (-> (w b) c), wa : w a] : (w c)
  (extend wa (fn [wa'] (g (extend wa' f)))))

;; Lift a function into the comonadic context
(defn co-lift [^Comonad w, a : *, b : *]
  [f : (-> a b), wa : w a] : (w b)
  (extend wa (fn [wa'] (f (extract wa')))))
```

### Standard Comonads

#### 2.1 Product Comonad (Pairs)

```clojure
;; The product comonad for pairs — each element "sees" both values
(definstance Comonad (Pair a)
  (extract [p] (first p))
  (extend [p, f] 
    (let [a (first p)
          b (second p)]
      (Pair. (f p) (f (Pair. b a))))))  ; swap for second element
```

#### 2.2 Environment Comonad (Reader)

```clojure
;; The environment/reader comonad — like a function (e -> a)
;; extract gets the value at the current environment
;; extend applies f to the function's result with different environments
(deftype Env [e a] (-> e a))

(definstance Functor Env
  (map [e a b] [f, g] (fn [env] (g (f env)))))

(definstance Comonad (Env e)
  (extract [a] [f] (f ???))  ; Need a default environment — tricky!
  (extend [a b] [f, g] (fn [env] (g (fn [env'] (f env'))))))
```

**Note:** The environment comonad requires a way to provide the "current" environment. In practice, this is often handled by making the comonad relative to a specific environment value.

#### 2.3 List Comonad (Zipper-like)

```clojure
;; A list with a current position (zipper)
;; This is the key comonad for cellular automata on 1D grids
(defstruct Zipper [
  left : (list a)   ; reversed elements to the left
  focus : a         ; current element
  right : (list a)]) ; elements to the right

(definstance Functor Zipper
  (map [a b] [z, f] 
    (Zipper. (map (left z) f) (f (focus z)) (map (right z) f))))

(definstance Comonad Zipper
  (extract [a] [z] (focus z))
  (extend [a b] [z, f]
    ;; For each position, create a new zipper focused there
    ;; and apply f to it
    (let [result (f z)]
      (Zipper. 
        (map (left z) (fn [x] (f (Zipper. (drop-last (left z)) x (cons (focus z) (right z))))))
        result
        (map (right z) (fn [x] (f (Zipper. (cons (focus z) (left z)) x (drop-first (right z)))))))))))
```

---

## 3. Grid as a Comonad

### Grid Representation

```clojure
;; stdlib/grid.tur

;; A 2D grid with a current position
;; Using a coordinate-based representation for efficiency
(defstruct Grid [
  width : int
  height : int
  data : (vec a)       ; row-major flat storage
  x : int              ; current x position
  y : int])             ; current y position

;; Helper: get value at position
(defn grid-get [^Comonad w, a : *] [g : w a, x : int, y : int] : a
  (extract (extend g (fn [g'] (if (and (= (x g') x) (= (y g') y)) (extract g') ???)))))
```

**Better approach:** The grid comonad should be a separate type that wraps the grid data and position:

```clojure
(deftype GridPos [a] (-> int int a))  ; Function from (x,y) to value

;; But this doesn't give us a current position. Let's use a different approach:

defstruct GridCtx [
  grid : (vec (vec a))   ; 2D grid data
  x : int                ; current x
  y : int]               ; current y

(definstance Functor GridCtx
  (map [a b] [g, f]
    (GridCtx. (map (grid g) (fn [row] (map row f))) (x g) (y g))))
```

### Grid Comonad Instance

```clojure
(definstance Comonad GridCtx
  (extract [a] [g] 
    (let [row ((grid g) (y g))]
      (row (x g))))
  
  (extend [a b] [g, f]
    ;; For each position (x', y'), create a GridCtx focused there and apply f
    (let [w (grid g)
          h (height g)
          result-grid (vec/new h)]
      (loop [y' 0, result result-grid]
        (if (= y' h)
          (GridCtx. result (x g) (y g))  ; Keep original position
          (let [row ((grid g) y'))
               new-row (loop [x' 0, new-row []]
                         (if (= x' (vec/len row))
                           new-row
                           (let [g' (GridCtx. (grid g) x' y')]
                             (recur (+ x' 1) (conj new-row (f g'))))))
                         result' (vec/set result y' new-row)]
                   (recur (+ y' 1) result')))))))))
```

**Problem:** This `extend` implementation is O(n²) and creates many intermediate GridCtx values. We need a more efficient approach.

### Efficient Grid Comonad

```clojure
;; Better: Pre-compute all positions once, then map
(definstance Comonad GridCtx
  (extract [a] [g] 
    (let [row ((grid g) (y g))]
      (row (x g))))
  
  (extend [a b] [g, f]
    ;; Create a new grid where each cell is f applied to the GridCtx at that position
    (let [w (width g)
          h (height g)
          new-grid (vec/new h)]
      (loop [y' 0, result new-grid]
        (if (= y' h)
          (GridCtx. result (x g) (y g))
          (let [row ((grid g) y')
                new-row (loop [x' 0, new-row []]
                          (if (= x' w)
                            new-row
                            (let [cell-ctx (GridCtx. (grid g) x' y')]
                              (recur (+ x' 1) (conj new-row (f cell-ctx))))))
                result' (vec/set result y' new-row)]
                  (recur (+ y' 1) result'))))))))
```

### Moore Neighborhood Helper

For Conway's Game of Life, we need to access the 8 neighboring cells:

```clojure
(defn moore-neighborhood [g : (GridCtx bool)] : (list bool)
  (let [w (width g)
        h (height g)
        x (x g)
        y (y g)
        grid-data (grid g)]
    (loop [dx -1, neighbors []]
      (if (> dx 1)
        neighbors
        (loop [dy -1, neighbors neighbors]
          (if (> dy 1)
            neighbors
            (let [nx (+ x dx)
                  ny (+ y dy)]
              (if (and (!= dx 0) (!= dy 0)  ; skip self
                       (>= nx 0) (< nx w)
                       (>= ny 0) (< ny h))
                (let [row (grid-data ny)
                      val (row nx)]
                  (recur (+ dy 1) (conj neighbors val)))
                (recur (+ dy 1) neighbors)))))))))

;; Count live neighbors
(defn live-neighbors [g : (GridCtx bool)] : int
  (count true (moore-neighborhood g)))
```

---

## 4. Conway's Game of Life Implementation


### The Rules

Conway's Game of Life has four rules:

1. **Underpopulation:** Any live cell with fewer than 2 live neighbors dies
2. **Overpopulation:** Any live cell with more than 3 live neighbors dies
3. **Survival:** Any live cell with exactly 2 or 3 live neighbors lives on
4. **Reproduction:** Any dead cell with exactly 3 live neighbors becomes alive

### Rule Function

```clojure
(defn game-of-life-rule [g : (GridCtx bool)] : bool
  (let [alive (extract g)
        live-count (live-neighbors g)]
    (cond
      (and alive (< live-count 2))  false  ; underpopulation
      (and alive (> live-count 3))  false  ; overpopulation
      (and alive (or (= live-count 2) (= live-count 3))) alive  ; survival
      (and (not alive) (= live-count 3)) true   ; reproduction
      :else alive)))
```

### Next Generation

```clojure
(defn next-generation [^Comonad w, a : *]
  [grid : w a, rule : (-> (w a) a)] : (w a)
  (extend grid rule))

;; For Conway's Game of Life:
defn life-step [grid : (GridCtx bool)] : (GridCtx bool)
  (next-generation grid game-of-life-rule))
```

### Running the Simulation

```clojure
(defn simulate [initial : (GridCtx bool), steps : int] : (GridCtx bool)
  (loop [grid initial, n steps]
    (if (= n 0)
      grid
      (recur (life-step grid) (- n 1)))))

;; Helper to create a grid from a pattern
(defn grid-from-pattern [pattern : (vec (vec bool))] : (GridCtx bool)
  (let [h (vec/len pattern)
        w (vec/len (pattern 0))]
    (GridCtx. pattern 0 0)))  ; Focus at (0,0) initially

;; Glider pattern
(def glider
  [[false false false false false]
   [false false true false false]
   [false false false true false]
   [false true true true false]
   [false false false false false]])

;; Run simulation
defn main [] : int
  (let [initial (grid-from-pattern glider)
        final (simulate initial 4)]
    (print-grid final)
    0))
```

### Optimization: In-Place Update

The above implementation creates a new grid for each generation. For better performance, we can use a double-buffering approach:

```clojure
defn life-step-in-place [grid : (GridCtx bool)] : (GridCtx bool)
  ;; Create a copy of the grid for reading
  (let [old-grid (GridCtx. (vec/clone (grid grid)) (x grid) (y grid))
        new-grid (extend old-grid game-of-life-rule)]
    new-grid))
```

---

## 5. Tutorial Structure

The tutorial will be structured as a **step-by-step guide** in `docs/guides/cellular-automata-comonad-tutorial.md`:

### Part 1: Introduction to Comonads (30 min)

**Goal:** Understand what comonads are and when to use them.

| Section | Content | Code Example |
|---|---|---|
| 1.1 What is a Comonad? | Dual of monads, intuition, use cases | Simple identity comonad |
| 1.2 Comonad Laws | extract, extend, duplicate relationships | Law verification |
| 1.3 Comonads in the Wild | Real-world examples | Product, Environment comonads |
| 1.4 Why Cellular Automata? | Motivation for this tutorial | Game of Life overview |

**Exercises:**
1. Implement a trivial comonad for a wrapper type
2. Prove the comonad laws for the identity comonad
3. Implement the Product comonad for pairs

### Part 2: Building the Grid Comonad (45 min)

**Goal:** Implement a 2D grid with a comonadic structure.

| Section | Content | Code Example |
|---|---|---|
| 2.1 Grid Representation | Choosing data structures | GridCtx definition |
| 2.2 Functor Instance | Mapping over all cells | map implementation |
| 2.3 Comonad Instance | extract and extend | GridCtx Comonad |
| 2.4 Neighborhood Access | Moore neighborhood helper | moore-neighborhood |
| 2.5 Visualization | Printing the grid | print-grid function |

**Exercises:**
1. Implement a 1D grid (zipper) comonad
2. Add Von Neumann neighborhood (4-directional)
3. Implement hexagonal grid neighborhood

### Part 3: Conway's Game of Life (30 min)

**Goal:** Implement Conway's Game of Life using the comonadic grid.

| Section | Content | Code Example |
|---|---|---|
| 3.1 The Rules | Underpopulation, survival, reproduction | Rule description |
| 3.2 Rule Function | Implementing game-of-life-rule | game-of-life-rule |
| 3.3 Next Generation | Using extend | next-generation |
| 3.4 Running Simulation | Loop with steps | simulate |
| 3.5 Patterns | Glider, spaceship, oscillator | Pattern definitions |

**Exercises:**
1. Implement different initial patterns (block, beehive, loaf)
2. Add boundary conditions (toroidal, infinite)
3. Count live cells in a generation

### Part 4: Beyond Game of Life (45 min)

**Goal:** Explore variations and other cellular automata.

| Section | Content | Code Example |
|---|---|---|
| 4.1 Rule Variations | Custom rules | rule builder |
| 4.2 Elementry CA | 1D cellular automata | 1D GridCtx |
| 4.3 Wireworld | Electron/conducter simulation | Wireworld rules |
| 4.4 Brian's Brain | Three-state automaton | Brian's Brain rules |
| 4.5 Performance | Optimization techniques | Double buffering |

**Exercises:**
1. Implement Rule 30 (1D CA)
2. Create a custom automaton with 4 states
3. Optimize the simulation using memoization

### Part 5: Advanced Topics (Optional, 30 min)

**Goal:** Explore advanced comonad concepts.

| Section | Content | Code Example |
|---|---|---|
| 5.1 Comonad Transformers | Stacking comonads | Cokleisli |
| 5.2 Coalgebras | Theoretical foundation | Coalgebra definition |
| 5.3 Co-Yoneda Lemma | Connection to category theory | co-yoneda |
| 5.4 Distributed CA | Parallel computation | Threading |
| 5.5 Comonad + Monad | Interaction | Store comonad |

**Exercises:**
1. Implement a comonad transformer for logging
2. Prove that GridCtx forms a coalgebra
3. Parallelize the simulation using threads

### Part 6: Real-World Applications (30 min)

**Goal:** Connect to practical use cases.

| Section | Content | Code Example |
|---|---|---|
| 6.1 Image Processing | Convolution with comonads | Image filter |
| 6.2 Physics Simulation | Particle systems | Force calculation |
| 6.3 Graph Algorithms | Comonadic graph traversal | Graph comonad |
| 6.4 Neural Networks | Weight updates | Layer comonad |

**Exercises:**
1. Implement a blur filter using comonads
2. Simulate particle interactions
3. Breadth-first search with comonads

---

## 6. Implementation Phases

### Phase CA0: Prerequisites Verification

**Goal:** Ensure all dependencies are in place.

- [ ] HKT Phase H3 complete (Functor, Applicative, Monad typeclasses)
- [ ] `stdlib/typeclass.tur` has Functor definition
- [ ] Typeclass constraints work in `defn` parameters
- [ ] Kind-polymorphic functions compile correctly

**Fixtures:**
- [ ] `comonad-prereq-functor.tur` — Functor typeclass works
- [ ] `comonad-prereq-kinds.tur` — Kind-polymorphic functions work

### Phase CA1: Comonad Typeclass

**Goal:** Implement the Comonad typeclass and basic instances.

#### Tasks

**`stdlib/comonad.tur`**
- [ ] Define `Comonad` typeclass (extends `Functor`)
- [ ] Implement `duplicate` from `extend`
- [ ] Implement `co-kleisli` composition
- [ ] Implement `co-lift`
- [ ] Add `cojoin` (dual of `join` for monads)

**Standard comonad instances:**
- [ ] `Identity` comonad
- [ ] `Pair` (Product) comonad
- [ ] `Env` (Environment/Reader) comonad
- [ ] `Zipper` for lists (1D grid)

**Fixtures:**
- [ ] `comonad-identity.tur` — Identity comonad laws
- [ ] `comonad-pair.tur` — Pair comonad instance
- [ ] `comonad-env.tur` — Environment comonad
- [ ] `comonad-zipper.tur` — Zipper comonad

**Exit criterion:** Comonad typeclass compiles; all basic instances pass law checks; fixtures green.

### Phase CA2: Grid Data Structure

**Goal:** Implement the 2D grid data structure with Functor and Comonad instances.

#### Tasks

**`stdlib/grid.tur`**
- [ ] Define `GridCtx` struct with grid data, width, height, x, y
- [ ] Helper functions: `grid/new`, `grid/get`, `grid/set`, `grid/clone`
- [ ] `Functor` instance for `GridCtx`
- [ ] `Comonad` instance for `GridCtx`
- [ ] Neighborhood helpers: `moore-neighborhood`, `von-neumann-neighborhood`
- [ ] Boundary condition helpers: `wrap-coords`, `clamp-coords`
- [ ] Visualization: `grid/print`

**Fixtures:**
- [ ] `grid-basic.tur` — Grid creation and access
- [ ] `grid-functor.tur` — Functor instance works
- [ ] `grid-comonad.tur` — Comonad instance works
- [ ] `grid-neighborhood.tur` — Neighborhood helpers

**Exit criterion:** Grid comonad compiles; neighborhood access works; fixtures green.

### Phase CA3: Game of Life Implementation

**Goal:** Implement Conway's Game of Life using the grid comonad.

#### Tasks

**`examples/cellular-automata.tur`**
- [ ] Define Conway's rules: `game-of-life-rule`
- [ ] Implement `next-generation` using `extend`
- [ ] Implement `simulate` loop
- [ ] Define common patterns: glider, block, beehive, etc.
- [ ] Add `main` with command-line argument parsing
- [ ] Add visualization options (text, ANSI colors)

**Fixtures:**
- [ ] `game-of-life-glider.tur` — Glider moves correctly
- [ ] `game-of-life-block.tur` — Block is stable
- [ ] `game-of-life-oscillator.tur` — Blinker oscillates
- [ ] `game-of-life-still-life.tur` — Still lifes remain unchanged

**Exit criterion:** Game of Life runs correctly for all test patterns; matches known behavior.

### Phase CA4: Tutorial Documentation

**Goal:** Write the comprehensive tutorial.

#### Tasks

**`docs/guides/cellular-automata-comonad-tutorial.md`**
- [ ] Part 1: Introduction to Comonads
- [ ] Part 2: Building the Grid Comonad
- [ ] Part 3: Conway's Game of Life
- [ ] Part 4: Beyond Game of Life
- [ ] Part 5: Advanced Topics
- [ ] Part 6: Real-World Applications
- [ ] Exercises with solutions
- [ ] Further reading references

**Exit criterion:** Tutorial is complete; all examples compile and run; exercises have solutions.

### Phase CA5: Polish & Integration

**Goal:** Final polish, testing, and integration.

#### Tasks

- [ ] Add `tur explain` support for comonad-related errors
- [ ] Performance benchmarks for different grid sizes
- [ ] ASan/UBSan clean for all examples
- [ ] Integration with existing stdlib
- [ ] Add to main README examples
- [ ] Create screenshots/animations for documentation

**Fixtures:**
- [ ] `game-of-life-perf.tur` — Performance benchmarks
- [ ] `game-of-life-asan.tur` — ASan clean
- [ ] `comonad-integration.tur` — Integration with other typeclasses

**Exit criterion:** All fixtures green; tutorial is polished; ready for merge.

---

## 7. Testing Strategy

### Unit Tests

Each component should have focused unit tests:

| Component | Test File | Coverage |
|---|---|---|
| Comonad typeclass | `tests/comonad-test.tur` | Laws, basic operations |
| Grid data structure | `tests/grid-test.tur` | Creation, access, bounds |
| Game of Life | `tests/game-of-life-test.tur` | Rule correctness |

### Property Tests

Use property-based testing where applicable:

```clojure
;; Comonad laws: extend f (extend g wa) = extend (f . extend g) wa
(defn test-comonad-law-1 [^Comonad w, a b c]
  [wa : w a, f : (-> (w a) b), g : (-> (w a) c)] : bool
  (let [lhs (extend wa (fn [wa'] (f (extend wa' g))))
        rhs (extend wa (fn [wa'] ((comp f (extend wa')) g)))]
    (= lhs rhs)))
```

### Known Pattern Tests

Verify against known cellular automata behavior:

| Pattern | Expected Behavior | Test |
|---|---|---|
| Block | Stable (doesn't change) | `test-block-stable` |
| Beehive | Stable | `test-beehive-stable` |
| Loaf | Stable | `test-loaf-stable` |
| Glider | Moves diagonally every 4 steps | `test-glider-movement` |
| Lightweight Spaceship | Moves every 4 steps | `test-lwss-movement` |
| Blinker | Oscillates with period 2 | `test-blinker-oscillation` |
| Toad | Oscillates with period 2 | `test-toad-oscillation` |
| Pulsar | Oscillates with period 3 | `test-pulsar-oscillation` |

### Performance Tests

Benchmark different implementations:

```clojure
;; Benchmark: time to compute N generations of a MxM grid
defn benchmark-game-of-life [size : int, generations : int] : double
  (let [grid (random-grid size size 0.3)  ; 30% live cells
        start (time/now)]
    (simulate grid generations)
    (time/since start)))
```

Target performance (on a modern machine):
- 10x10 grid, 100 generations: < 1ms
- 100x100 grid, 100 generations: < 100ms
- 1000x1000 grid, 100 generations: < 10s

---

## 8. Performance Considerations

### Memory Usage

| Approach | Memory | Pros | Cons |
|---|---|---|---|
| Naive (new grid each step) | O(2 * width * height) | Simple | High memory churn |
| Double buffering | O(2 * width * height) | No allocation per step | Still 2x memory |
| In-place (impossible for CA) | O(width * height) | Minimal | Can't read while writing |

**Recommendation:** Use double buffering (two grids, swap each generation).

### Computation

| Operation | Complexity | Notes |
|---|---|---|
| Moore neighborhood | O(1) per cell | 8 neighbors, bounds checking |
| Next generation | O(width * height) | Must visit every cell |
| extend | O(width * height) | Creates new grid |
| Full simulation | O(generations * width * height) | Dominated by generation count |

### Optimization Opportunities

1. **Sparse grids:** Only store live cells (for sparse patterns)
2. **Bit-packing:** Store bool grids as bits in integers
3. **SIMD:** Use vector instructions for neighborhood counting
4. **Parallelism:** Process independent regions in parallel
5. **Hashlife:** Memoization-based algorithm (advanced)

### Bit-Packed Grid

```clojure
;; Store grid as bits in uint64_t values
;; 64 cells per uint64_t
(deftype BitGrid [
  width : int
  height : int
  cells : (vec uint64_t)])

(defn bitgrid-get [g : BitGrid, x : int, y : int] : bool
  (let [idx (+ (* y (width g)) x)
        word-idx (/ idx 64)
        bit-idx (mod idx 64)]
    (not= 0 (bit-and (nth (cells g) word-idx) (bit-shift-left 1 bit-idx)))))

;; Comonad instance for BitGrid would need to handle bit manipulation
```

---

## 9. Extensions

### 9.1 Other Cellular Automata

#### Rule 30 (1D)

```clojure
(defn rule-30 [left : bool, center : bool, right : bool] : bool
  (let [neighborhood (bit-pack left center right)]
    (case neighborhood
      0b000 false  ; 000 -> 0
      0b001 true   ; 001 -> 1
      0b010 true   ; 010 -> 1
      0b011 true   ; 011 -> 1
      0b100 true   ; 100 -> 1
      0b101 false  ; 101 -> 0
      0b110 false  ; 110 -> 0
      0b111 false))) ; 111 -> 0

(defn rule-30-step [grid : (GridCtx 1 bool)] : (GridCtx 1 bool)
  (extend grid (fn [g]
    (let [x (x g)
          left (grid-get g (- x 1) (y g))
          center (extract g)
          right (grid-get g (+ x 1) (y g))]
      (rule-30 left center right)))))
```

#### Wireworld

Four states: empty, conductor, electron head, electron tail.

```clojure
(deftype WireState []
  (Empty)
  (Conductor)
  (ElectronHead)
  (ElectronTail))

(defn wireworld-rule [g : (GridCtx WireState)] : WireState
  (let [center (extract g)
        neighbors (moore-neighborhood g)]
    (match center
      (Empty) (Empty)
      (Conductor) 
        (let [head-count (count (fn [s] (match s (ElectronHead) true _ false)) neighbors)]
          (if (= head-count 1) (ElectronHead) (Conductor)))
      (ElectronHead) (ElectronTail)
      (ElectronTail) (Conductor))))
```

#### Brian's Brain

Three states: off, on, dying.

```clojure
(deftype BrainState []
  (Off)
  (On)
  (Dying))

(defn brains-brain-rule [g : (GridCtx BrainState)] : BrainState
  (let [center (extract g)
        neighbors (moore-neighborhood g)
        on-count (count (fn [s] (match s (On) true _ false)) neighbors)]
    (match center
      (Off) (if (= on-count 2) (On) (Off))
      (On) (On)
      (Dying) (Off))))
```

### 9.2 Comonad Transformers

#### Store Comonad (Comonadic State)

The Store comonad combines a comonad with a state that can be updated:

```clojure
;; Store w s a = (w a, s) with special comonadic structure
;; This allows threading state through comonadic operations
```

#### Traced Comonad

Tracks the path taken through the comonad:

```clojure
;; Traced w a = (w a, [path]) where path is the sequence of operations
```

### 9.3 3D and Higher-Dimensional Grids

```clojure
(defstruct Grid3D [
  width : int
  height : int
  depth : int
  data : (vec (vec (vec a)))
  x : int
  y : int
  z : int])

;; 26-cell neighborhood for 3D Moore neighborhood
defn moore-neighborhood-3d [g : (Grid3DCtx bool)] : (list bool)
  ...
```

### 9.4 Toroidal (Wrapping) Grids

```clojure
(defn wrap-coords [g : GridCtx, x : int, y : int] : [int int]
  [(mod (+ x (width g)) (width g)) (mod (+ y (height g)) (height g))])

(defn moore-neighborhood-toroidal [g : (GridCtx bool)] : (list bool)
  (loop [dx -1, neighbors []]
    (if (> dx 1)
      neighbors
      (loop [dy -1, neighbors neighbors]
        (if (> dy 1)
          neighbors
          (let [[nx ny] (wrap-coords g (+ (x g) dx) (+ (y g) dy))
                val (grid-get g nx ny)]
            (if (or (!= dx 0) (!= dy 0))
              (recur (+ dy 1) (conj neighbors val))
              (recur (+ dy 1) neighbors)))))))))
```

### 9.5 Infinite Grids

Use a sparse representation for infinite grids:

```clojure
;; Only store live cells in a hash map
deftype SparseGrid [a]
  live-cells : (HashMap [int int] a)  ; (x,y) -> a
  default : a                            ; value for empty cells

;; Comonad instance would need to handle the sparse structure
;; and provide the illusion of an infinite grid
```

### 9.6 Graph Comonad

Treat graphs as comonads where each node is the "current position":

```clojure
(defstruct GraphCtx [
  graph : Graph
  node : NodeId])

(definstance Comonad GraphCtx
  (extract [a] [g] (graph/get-node (graph g) (node g)))
  (extend [a b] [g, f]
    ;; For each node in the graph, create a GraphCtx and apply f
    ...))
```

---

## Typeclass Dependencies

```
stdlib/comonad.tur
├── Comonad typeclass (extends Functor)
│   ├── extract
│   └── extend
├── co-lift
├── co-kleisli
└── cojoin

stdlib/grid.tur
├── GridCtx struct
├── Functor GridCtx instance
├── Comonad GridCtx instance
├── Neighborhood helpers
│   ├── moore-neighborhood
│   ├── von-neumann-neighborhood
│   └── hexagonal-neighborhood
└── Visualization
    └── grid/print

examples/cellular-automata.tur
├── Conway's Game of Life
│   ├── game-of-life-rule
│   ├── life-step
│   └── simulate
├── Rule 30 (1D)
│   └── rule-30-step
├── Wireworld
│   └── wireworld-rule
├── Brian's Brain
│   └── brains-brain-rule
└── Main entry point

Docs
└── docs/guides/cellular-automata-comonad-tutorial.md
    ├── Part 1: Introduction to Comonads
    ├── Part 2: Building the Grid Comonad
    ├── Part 3: Conway's Game of Life
    ├── Part 4: Beyond Game of Life
    ├── Part 5: Advanced Topics
    └── Part 6: Real-World Applications
```

---

## File Structure

```
.
├── docs/
│   └── guides/
│       ├── cellular-automata-comonad-tutorial.md  ; Main tutorial
│       └── cellular-automata-comonad-exercises.md    ; Exercise solutions
├── stdlib/
│   ├── comonad.tur                  ; Comonad typeclass
│   └── grid.tur                     ; Grid data structure + Comonad instance
├── examples/
│   └── cellular-automata.tur        ; Main example with Game of Life
├── tests/
│   ├── comonad-test.tur             ; Comonad typeclass tests
│   ├── grid-test.tur                ; Grid tests
│   └── game-of-life-test.tur         ; Game of Life tests
└── README.md                        ; Update with example link
```

---

## Prerequisites Checklist

Before starting Phase CA0, verify:

- [ ] HKT Phase H0 (Kind system) complete
- [ ] HKT Phase H1 (Kind-polymorphic typeclasses) complete
- [ ] HKT Phase H3 (Functor, Applicative, Monad) complete
- [ ] `stdlib/typeclass.tur` has `Functor` definition
- [ ] Typeclass constraints work in `defn` parameters
- [ ] Kind-polymorphic functions compile correctly
- [ ] `vec` type is available in stdlib
- [ ] `HashMap` or similar is available (for sparse grids)

---

## Open Questions

1. **Should `extend` or `duplicate` be primitive?**
   - `extend` is more efficient for many comonads
   - `duplicate` is more fundamental (can derive `extend`)
   - **Recommendation:** `extend` as primitive, derive `duplicate`

2. **How to handle grid boundaries?**
   - Clamped (default to edge value)
   - Wrapped (toroidal)
   - Infinite (sparse representation)
   - **Recommendation:** Support all three via configuration

3. **Should the grid be mutable or immutable?**
   - Immutable: Pure functional, easier to reason about
   - Mutable: Better performance for in-place updates
   - **Recommendation:** Immutable by default, with mutable variant for performance

4. **How to represent the grid data?**
   - `vec (vec a)`: Simple, but inefficient for sparse grids
   - Flat `vec a`: Better cache locality
   - Custom struct: Most flexible
   - **Recommendation:** Flat `vec a` with row-major layout

5. **Should we support different neighborhood types?**
   - Moore (8-directional)
   - Von Neumann (4-directional)
   - Hexagonal
   - Custom (user-defined)
   - **Recommendation:** Yes, with Moore as default

6. **How to handle the "current position" in the comonad?**
   - Store (x,y) coordinates in GridCtx
   - Use a cursor/zipper approach
   - **Recommendation:** Store (x,y) coordinates

7. **Should the comonad instance be lawful?**
   - Comonad laws: `extract . duplicate = id` and `fmap extract . duplicate = id` and `duplicate . fmap extract = extend id`
   - **Recommendation:** Yes, verify laws in tests

---

## Resolved Decisions

| Decision | Resolution | Rationale |
|---|---|---|
| Primitive operation | `extend` | More efficient for grid comonad |
| Grid representation | Flat `vec a` with row-major | Best cache locality |
| Boundary handling | Configurable | Flexibility for different use cases |
| Immutability | Immutable by default | Pure functional, easier to reason |
| Neighborhood types | Support multiple | Different automata need different neighborhoods |
| Current position | Stored (x,y) coordinates | Simple and intuitive |
| Lawfulness | Verify in tests | Ensures correctness |

---

## References

### Comonads
- [Comonads as Objects — Uustalu & Vene](https://arxiv.org/abs/cs/0208021)
- [The Essence of Dataflow Programming — Uustalu & Vene](https://arxiv.org/abs/0903.0756)
- [Comonads for Modeling Dataflow](https://blog.sigfpe.com/2011/07/comonads-for-modeling-dataflow.html)
- [Haskell Wiki: Comonad](https://wiki.haskell.org/wiki/Comonad)

### Cellular Automata
- [Conway's Game of Life — Wikipedia](https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life)
- [A New Kind of Science — Wolfram](https://www.wolframscience.com/nks/)
- [Golly — Game of Life Simulator](http://golly.sourceforge.net/)

### Cellular Automata + Comonads
- [Comonads and Cellular Automata — Haskell Wiki](https://wiki.haskell.org/wiki/Comonad/Cellular_automata)
- [Cellular Automata are Comonads — Blog Post](https://chrispenner.ca/posts/cellular-automata-are-comonads)
- [Adventures in Comonad Land — Blog Series](https://blog.sigfpe.com/2011/06/adventures-in-comonad-land.html)

### Typeclass Hierarchy
- [Functor, Applicative, Monad, Comonad — Haskell Hierarchy](https://wiki.haskell.org/wiki/Typeclass_hierarchy)
- [The Comonad.Reader Newtype — Haskell](https://hackage.haskell.org/package/comonad-5.0.8/docs/Control-Comonad-Reader.html)

---

## Estimated Timeline

| Phase | Duration | Dependencies |
|---|---|---|
| CA0 | 0.5–1 week | HKT H3 complete |
| CA1 | 1–2 weeks | CA0 complete |
| CA2 | 1–2 weeks | CA1 complete |
| CA3 | 1–2 weeks | CA2 complete |
| CA4 | 2–3 weeks | CA3 complete |
| CA5 | 1–2 weeks | CA4 complete |
| **Total** | **7–12 weeks** | |

---

*Last updated: 2026-05-11*
