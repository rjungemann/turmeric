# Cellular Automata Comonad Tutorial — Prerequisites

**Purpose:** Define the prerequisite tasks that must be completed before implementing Phases CA0–CA5.

**Current Status:** Blocked by typeclass infrastructure and compiler limitations.

---

## Table of Contents

1. [Dependency Graph](#dependency-graph)
2. [Prerequisite Tasks (P-Tasks)](#prerequisite-tasks)
3. [Validation Fixtures](#validation-fixtures)
4. [Implementation Order](#implementation-order)

---

## Dependency Graph

```
CA1 (Comonad Typeclass)
  ↑
  └─ P2: Fix Return Type Support
       └─ P1: Fix typeclass.tur Compilation

CA2 (Grid Comonad)
  ↑
  ├─ CA1 ✓
  ├─ P3: Get vec.tur Compiling
  │   └─ P1 ✓
  └─ P4: Generic Struct Support

CA3 (Game of Life)
  ↑
  ├─ CA2 ✓
  └─ P5: Standard Library Integration

CA4 (Tutorial)
  ↑
  └─ CA1, CA2, CA3 ✓

CA5 (Polish)
  ↑
  └─ CA4 ✓
```

---

## Prerequisite Tasks

### P1: Fix `stdlib/typeclass.tur` Compilation Errors

**Priority:** CRITICAL (blocks everything)

**Current State:** `stdlib/typeclass.tur` fails to compile with:
```
Error: unsupported return type :cstr (Error typeclass error-message method)
Error: unsupported return type :ptr<void> (Error typeclass error-cause method, From typeclass)
```

**Problem:** The language only supports `:int` as a return type in most contexts. Other types cause compilation failures.

#### Tasks

1. **Understand the limitation**
   - [ ] Read `src/elab.c` — search for "unsupported return type" error message
   - [ ] Identify which compiler pass enforces this restriction
   - [ ] Determine if it's a fundamental language limitation or a TODO

2. **Option A: Extend Compiler Support (Preferred)**
   - [ ] Add support for `:cstr` return type in elaboration pass
   - [ ] Add support for `:ptr<void>` return type in emission pass
   - [ ] Ensure generated C code is valid for all types
   - [ ] Create test fixtures: `tests/fixtures/return-cstr/`, `tests/fixtures/return-ptr-void/`
   - **Effort:** Medium (compiler work)
   - **Benefit:** Unblocks all HKT-dependent code

3. **Option B: Refactor typeclass.tur (Workaround)**
   - [ ] Remove `:cstr` return type from Error typeclass
   - [ ] Replace error-message method with `:int` error code
   - [ ] Remove `:ptr<void>` from From typeclass
   - [ ] Use `:int` handles instead of raw pointers
   - **Effort:** Low (library refactoring)
   - **Benefit:** Gets typeclass.tur compiling immediately
   - **Cost:** Reduces expressiveness; may need separate error handling library

**Exit Criterion:** `stdlib/typeclass.tur` compiles without errors.

**Test:** 
```bash
./build/tur emit-c stdlib/typeclass.tur 2>&1 | grep -i error
# Should have no output (no errors)
```

---

### P2: Implement Kind-Polymorphic Function Support

**Priority:** HIGH (needed for P1 Option A and CA1)

**Current State:** Unknown if kind-polymorphic functions work. Need to verify.

**Problem:** Typeclass methods like `extract`, `extend`, `duplicate` must work with types of different kinds (e.g., `(w a)` where `w` is a type constructor).

#### Tasks

1. **Research current support**
   - [ ] Check if `defn` supports higher-kinded polymorphism currently
   - [ ] Check if type parameters with `:*` kind work in method signatures
   - [ ] Look at `stdlib/comonad.tur` v1 (if it exists) for examples

2. **Implement if missing**
   - [ ] Extend `src/elab.c` to handle kind annotations (`: * -> *`)
   - [ ] Ensure elaboration correctly processes kind-polymorphic method signatures
   - [ ] Ensure emission generates correct C function signatures
   - [ ] Add test fixtures for kind polymorphism

3. **Create validation fixtures**
   - [ ] `tests/fixtures/kind-polymorphic-simple/` — Simple generic function
   - [ ] `tests/fixtures/kind-polymorphic-nested/` — Nested generic types

**Exit Criterion:** Kind-polymorphic functions compile and run correctly.

**Test:**
```bash
TUR_TEST_FILTER=kind-polymorphic bash tests/run.sh
# Should pass
```

---

### P3: Get `stdlib/vec.tur` Compiling

**Priority:** HIGH (CA2 depends on this)

**Current State:** `stdlib/vec.tur` fails because it tries to define instances for typeclasses that don't exist.

**Problem:** Cascading failure from P1. Once typeclass.tur compiles, vec.tur should too, but might have other issues.

#### Tasks

1. **Verify P1 is complete**
   - [ ] `stdlib/typeclass.tur` compiles cleanly
   - [ ] `stdlib/functor.tur` exists and compiles (if separate)

2. **Attempt to compile vec.tur**
   - [ ] Run: `./build/tur emit-c stdlib/vec.tur`
   - [ ] Document any errors
   - [ ] If errors, determine if they're:
     - Typeclass-related (solved by P1) ✓
     - Generic type issues (might need P2)
     - Other language features

3. **Fix any remaining issues**
   - [ ] Implement missing language features
   - [ ] Refactor vec.tur if needed
   - [ ] Add test fixtures: `tests/fixtures/vec-basic/`, `tests/fixtures/vec-functor/`

4. **Validate standard library functions**
   - [ ] `vec-new`, `vec-set`, `vec-get` work correctly
   - [ ] Functor instance works: `vec-map`
   - [ ] Collection operations work: `vec-fold`, `vec-filter`

**Exit Criterion:** `stdlib/vec.tur` compiles; all vector operations work.

**Test:**
```bash
TUR_TEST_FILTER=vec bash tests/run.sh
# Should pass all vector tests
```

---

### P4: Implement Generic Struct/Record Support

**Priority:** HIGH (CA2 needs GridCtx struct)

**Current State:** Unknown if generic structs are supported.

**Problem:** `GridCtx [a]` (generic grid type parameterized by element type) may not be supported.

#### Tasks

1. **Research struct support**
   - [ ] Can `deftype GridCtx [a]` be declared?
   - [ ] Can methods access `a` in generic structs?
   - [ ] Does emission generate correct C struct definitions?
   - [ ] Check `src/elab.c` and `src/emit.c` for struct handling

2. **Implement if missing**
   - [ ] Extend parser/elaborator to support generic struct type parameters
   - [ ] Ensure type checking handles generic structs correctly
   - [ ] Generate correct C code for generic structs
   - [ ] Add test fixtures

3. **Create validation fixtures**
   - [ ] `tests/fixtures/generic-struct-simple/` — `deftype Box [a]`
   - [ ] `tests/fixtures/generic-struct-methods/` — Methods on generic structs
   - [ ] `tests/fixtures/generic-struct-nested/` — Nested generics

**Exit Criterion:** Generic structs compile and run; methods work correctly.

**Test:**
```bash
TUR_TEST_FILTER=generic-struct bash tests/run.sh
# Should pass
```

---

### P5: Add Functor/Applicative/Monad Standard Typeclasses

**Priority:** HIGH (foundation for CA1)

**Current State:** `stdlib/typeclass.tur` defines the base Error/From typeclasses, but Functor/Applicative/Monad may not be defined yet.

**Problem:** Before implementing Comonad (which extends Functor), Functor must be available.

#### Tasks

1. **Verify Functor is defined**
   - [ ] Check if `stdlib/typeclass.tur` or separate file has Functor
   - [ ] Verify Functor has correct methods: `map`, `fmap`
   - [ ] Check signature matches standard expectations

2. **Implement Applicative (if missing)**
   - [ ] Define `Applicative [f : * -> *]` extending `Functor`
   - [ ] Methods: `pure` and `ap` (or `<*>`)
   - [ ] Add to `stdlib/applicative.tur` or similar

3. **Implement Monad (if missing)**
   - [ ] Define `Monad [m : * -> *]` extending `Applicative`
   - [ ] Methods: `return` and `bind` (or `>>=`)
   - [ ] Add to `stdlib/monad.tur` or similar

4. **Create instance for common types**
   - [ ] Identity monad instance
   - [ ] Option monad instance
   - [ ] List monad instance
   - [ ] Add test fixtures

**Exit Criterion:** Functor/Applicative/Monad available and working.

**Test:**
```bash
TUR_TEST_FILTER=functor bash tests/run.sh
TUR_TEST_FILTER=applicative bash tests/run.sh
TUR_TEST_FILTER=monad bash tests/run.sh
# All should pass
```

---

### P6: Implement Comonad Dual Operations

**Priority:** MEDIUM (needed before full CA1)

**Current State:** Unknown if co-operations are thought about.

**Problem:** Comonad has dual operations to Monad. May need special syntax or methods.

#### Tasks

1. **Decide on method names and signatures**
   - [ ] Use Haskell conventions (`extract`, `extend`, `duplicate`)?
   - [ ] Or domain-specific names?
   - [ ] Document choice in `stdlib/comonad.tur`

2. **Implement comonadic operations**
   - [ ] `extract :: w a -> a`
   - [ ] `extend :: (w a -> b) -> w a -> w b`
   - [ ] `duplicate :: w a -> w (w a)` (derived from extend + fmap)
   - [ ] `co-kleisli` composition

3. **Create test fixtures**
   - [ ] `tests/fixtures/comonad-identity/` — Identity comonad
   - [ ] `tests/fixtures/comonad-pair/` — Product comonad
   - [ ] `tests/fixtures/comonad-laws/` — Comonad law verification

**Exit Criterion:** Comonad typeclass implemented; basic instances work.

**Test:**
```bash
TUR_TEST_FILTER=comonad bash tests/run.sh
# Should pass all comonad tests
```

---

### P7: Implement List/Vector Comonad (1D Grid)

**Priority:** MEDIUM (stepping stone to 2D grid)

**Current State:** Not started.

**Problem:** Before implementing 2D grid comonad (GridCtx), 1D zipper comonad is a simpler test case.

#### Tasks

1. **Define Zipper data structure**
   - [ ] `deftype Zipper [a]` with left, focus, right
   - [ ] Implement as-you-go list with cursor

2. **Implement Functor instance**
   - [ ] Map over all elements including focus

3. **Implement Comonad instance**
   - [ ] `extract` — return focused element
   - [ ] `extend` — apply function to zipper at each position
   - [ ] May need to generate zippers for each position

4. **Add neighborhood helpers**
   - [ ] Left/right neighbors
   - [ ] All elements within distance N

5. **Create test fixtures**
   - [ ] `tests/fixtures/zipper-basic/`
   - [ ] `tests/fixtures/zipper-comonad/`

**Exit Criterion:** Zipper comonad works; can simulate 1D cellular automata.

**Test:**
```bash
TUR_TEST_FILTER=zipper bash tests/run.sh
# Should pass
```

---

### P8: Implement Memory Layout & Pointer Operations

**Priority:** MEDIUM (CA2 needs efficient grid storage)

**Current State:** Unknown if pointer arithmetic is exposed or if `inline-c` is the only way.

**Problem:** Efficient 2D grid needs row-major flat array. Turmeric needs way to access elements by index.

#### Tasks

1. **Understand current pointer model**
   - [ ] What return types can functions have? (P1 should expand this)
   - [ ] Can `defn` return `:ptr<T>` for generic types?
   - [ ] Is pointer arithmetic available?
   - [ ] Is `inline-c` blocks allowed in function bodies?

2. **If needed, implement pointer operations**
   - [ ] `ptr-add :: ptr T -> int -> ptr T` (pointer arithmetic)
   - [ ] `ptr-deref :: ptr T -> T` (dereference)
   - [ ] `ptr-set :: ptr T -> T -> nil` (assignment)
   - [ ] Or use `inline-c` for performance-critical paths

3. **Test with flat arrays**
   - [ ] Create fixture: `tests/fixtures/flat-array-access/`
   - [ ] Test row-major indexing: `index(x, y) = y * width + x`

**Exit Criterion:** Can efficiently access 2D flat array with pointer arithmetic or `inline-c`.

**Test:**
```bash
TUR_TEST_FILTER=flat-array bash tests/run.sh
# Should pass
```

---

## Validation Fixtures

Create test fixtures for each prerequisite. Naming convention: `tests/fixtures/P{N}-{description}/`

| Fixture | Purpose | P-Task |
|---------|---------|--------|
| `P1-typeclass-error` | Verify Error typeclass compiles | P1 |
| `P1-typeclass-from` | Verify From typeclass compiles | P1 |
| `P2-kind-polymorphic-simple` | Simple kind-polymorphic function | P2 |
| `P2-kind-polymorphic-nested` | Nested generic types | P2 |
| `P3-vec-basic` | Vector creation and access | P3 |
| `P3-vec-functor` | Vector Functor instance | P3 |
| `P4-generic-struct-simple` | `deftype Box [a]` | P4 |
| `P4-generic-struct-methods` | Methods on generic structs | P4 |
| `P5-functor-identity` | Functor on Identity | P5 |
| `P5-monad-identity` | Monad on Identity | P5 |
| `P6-comonad-identity` | Comonad on Identity | P6 |
| `P7-zipper-basic` | Zipper list creation | P7 |
| `P7-zipper-comonad` | Zipper Comonad instance | P7 |
| `P8-flat-array-access` | 2D array row-major indexing | P8 |

Each fixture should:
- Compile without errors
- Run without crashes
- Produce expected output
- Pass ASan/UBSan checks

---

## Implementation Order

### Phase 1: Compiler Foundation (Unblocks CA1)

**Goal:** Fix compiler to support all needed types and features.

1. **P1: Fix typeclass.tur** — Get Functor available
   - Duration: 1-2 days
   - Blockers: None
   - Then: P2, P3, P5

2. **P2: Kind-polymorphic support** — Support `[w : * -> *]`
   - Duration: 2-3 days
   - Blockers: P1
   - Then: P6

3. **P5: Functor/Applicative/Monad** — Standard typeclasses
   - Duration: 1 day
   - Blockers: P1, P2
   - Then: P6

### Phase 2: Higher-Kinded Types (Unblocks CA1-CA2)

**Goal:** Get full HKT infrastructure working.

4. **P6: Comonad typeclass** — Implement dual of Monad
   - Duration: 1-2 days
   - Blockers: P1, P2, P5
   - Then: CA1

5. **P3: vec.tur compilation** — Get standard vectors working
   - Duration: 1 day
   - Blockers: P1
   - Then: P7, CA2

6. **P4: Generic struct support** — Support `[a]` in structs
   - Duration: 1-2 days
   - Blockers: P2
   - Then: CA2

### Phase 3: Grid Infrastructure (Unblocks CA2-CA3)

**Goal:** Implement building blocks for grid comonad.

7. **P7: Zipper comonad** — 1D cellular automata
   - Duration: 1 day
   - Blockers: P3, P6
   - Then: CA2 (design reference)

8. **P8: Pointer operations** — Efficient array access
   - Duration: 1-2 days (if needed; may use `inline-c`)
   - Blockers: P1
   - Then: CA2

### Phase 4: Cellular Automata (Final)

**Goal:** Implement full tutorial as planned.

9. **CA1: Comonad typeclass** — Already enabled by Phase 2
10. **CA2: Grid comonad** — Already enabled by Phase 3
11. **CA3: Game of Life** — Use grid comonad
12. **CA4: Tutorial** — Document everything
13. **CA5: Polish** — Performance, integration

---

## Critical Path

**Minimum prerequisites to start CA1:**
1. P1 ✓ (typeclass.tur compiles)
2. P2 ✓ (kind-polymorphic functions)
3. P5 ✓ (Functor/Applicative/Monad)
4. P6 ✓ (Comonad typeclass)

**Minimum prerequisites to start CA2:**
5. P3 ✓ (vec.tur works)
6. P4 ✓ (generic structs)
7. P8 ✓ (pointer operations or `inline-c`)

**Minimum prerequisites to start CA3:**
- CA1 ✓
- CA2 ✓

---

## Related Documents

- [cellular-automata-comonad-tutorial-plan.md](cellular-automata-comonad-tutorial-plan.md) — Main implementation plan
- [cellular-automata-implementation-status.md](guides/cellular-automata-implementation-status.md) — Current status (working non-HKT version)
- [deferred-tasks-phase15-phase19.md](deferred-tasks-phase15-phase19.md) — Broader roadmap
