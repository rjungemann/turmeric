# Higher-Kinded Types (HKT) Implementation Plan for Turmeric

> **Status:** Draft — Not Started  
> **Prerequisite:** Phase 15 (Typeclasses) must be complete  
> **Target:** v2 or later  
> **Related:** See [turmeric-plan.md §12.2.1](turmeric-plan.md) for background and design constraints

---

## Executive Summary

Higher-Kinded Types (HKTs) enable type constructors that abstract over type parameters of arbitrary kinds. In Turmeric, this primarily means supporting typeclasses that quantify over type constructors (e.g., `Functor [f]` where `f` is `* -> *`), not just types. The v1 typeclass system is intentionally restricted to kind-`*` only; HKTs lift that restriction.

**Primary motivators:**
1. Generic `traverse` / `sequence` / `mapM` for monadic containers
2. Monad transformer libraries (`StateT`, `ExceptT`, `ReaderT`)
3. Free-monad / freer-monad encodings
4. Library-level abstractions that compose across containers

**Decision rule (from §12.2.1):** Ship only if ≥2 of: (1) users writing per-monad boilerplate, (2) library authors blocked on monad transformers, (3) significant demand from Haskell/Scala/PureScript users.

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| H0 | Kind system foundation | Kind annotations, kind inference, kind checking pass | Medium (1–2 weeks) |
| H1 | Kind-polymorphic typeclasses | `defclass` with kind-`* -> *` parameters, kind-constrained instances | Medium (2–3 weeks) |
| H2 | HKT dispatch table | Dispatch keyed on constructor + inner types, two-level lookup | Medium (1–2 weeks) |
| H3 | Built-in HKT typeclasses | `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` | Medium (2–3 weeks) |
| H4 | Kind-polymorphic functions | `defn` with kind-variable parameters, kind inference | Hard (3–4 weeks) |
| H5 | Advanced kinds | `* -> * -> *` (binary type constructors), kind aliases | Hard (2–3 weeks) |
| H6 | Integration & polish | Documentation, stdlib migration, performance benchmarks | Medium (1–2 weeks) |

---

## Prerequisites Checklist

Before starting Phase H0, verify:

- [ ] Phase 15 (Typeclasses, kind-`*` only) is complete and stable
- [ ] `Type` struct in `src/types.h` has a `kind` field (reserved in v1)
- [ ] Type variables carry an explicit kind slot (defaulting to `*`)
- [ ] No punning on type-constructor names (`option` vs `(option int)` are distinct in IR)
- [ ] Dispatch-table key is a struct, not a tuple-of-strings
- [ ] Names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` are reserved in typeclass namespace

---

## Phase H0 — Kind System Foundation

**Goal:** Add explicit kind support to the type system. Kinds classify types: `*` for proper types, `* -> *` for unary type constructors, `* -> * -> *` for binary, etc.

### Tasks

#### Type system extensions (`src/types.h`)
- [ ] Define `Kind` enum: `KIND_STAR` (`*`), `KIND_ARROW` (`k1 -> k2`)
- [ ] Add `Kind` field to `TypeVar` struct (replaces the reserved slot from v1)
- [ ] Add `Kind` field to `Type` struct for concrete types
- [ ] Define kind constants: `KIND_STAR`, `KIND_FUNCTOR` (`* -> *`), `KIND_BIFUNCTOR` (`* -> * -> *`)
- [ ] Add helper functions: `kind_eq()`, `kind_to_string()`, `kind_parse()`

#### Kind syntax (`src/reader.c`)
- [ ] Reserve `: *` as kind annotation suffix (e.g., `f : * -> *`)
- [ ] Reserve `^*` prefix for kind-variable binding in `defn`/`defclass` (e.g., `(defn map [^f : * -> *] ...)`)
- [ ] Error on use of kind syntax in v1 mode (feature flag)

#### Kind inference pass (`src/kind_check.c` — new file)
- [ ] Walk all types in a compilation unit
- [ ] Assign kinds to type variables based on use-site constraints
- [ ] For type constructors: `option` has kind `* -> *`, `pair` has kind `* -> * -> *`
- [ ] For applied types: `(option int)` has kind `*`
- [ ] Report kind errors: "expected kind `* -> *`, got `*`"
- [ ] Support kind unification for typeclass resolution

#### Built-in kinded types
- [ ] Annotate stdlib type constructors with kinds:
  - `option : * -> *`
  - `result : * -> * -> *`
  - `vec : * -> *`
  - `pair : * -> * -> *`
  - `slice : * -> *`
  - `ref : * -> *`
  - `rc : * -> *`

### Fixtures
- [ ] `kinds-basic.tur` — kind annotations parse correctly
- [ ] `kinds-inference.tur` — kinds inferred for common type constructors
- [ ] `kinds-error.tur` — kind mismatch produces diagnostic
- [ ] Codegen snapshot: kinds produce no runtime code

### Exit criterion
All kind fixtures green; kind checking pass runs clean; no performance regression on kind-`*` code.

---

## Phase H1 — Kind-Polymorphic Typeclasses

**Goal:** Extend `defclass` to accept type parameters of arbitrary kinds, not just `*`.

### Tasks

#### Typeclass extensions (`src/typeclass.c`)
- [ ] Modify `TypeClassParam` to store a `Kind` alongside the name
- [ ] Modify `TypeClass` struct to track kind of each parameter
- [ ] Add kind constraint validation: type argument must match parameter kind
- [ ] Extend instance application: `(definstance Functor (Option a) ...)` where `Option : * -> *`

#### Surface syntax (`src/reader.c`, `src/elab.c`)
- [ ] `(defclass Functor [f : * -> *] (map [a : *, b : *] [f a : f a, fn : a -> b] : f b) ...)`
- [ ] `(definstance Functor option (map [a b] [opt_a f] ...))`
- [ ] Kind annotation sugar: `(defclass Functor [f] ...)` infers `f : * -> *` from usage

#### Kind constraint propagation
- [ ] When resolving a typeclass constraint, verify kind compatibility
- [ ] Example: `Functor f` requires `f : * -> *`
- [ ] Error: "cannot implement `Functor` for `int` (expected kind `* -> *`, got `*`)"

#### Reserved typeclass names
- [ ] Reserve `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` in typeclass namespace
- [ ] Emit "reserved for HKT typeclass" error if user tries to define them before Phase H3

### Fixtures
- [ ] `hkt-typeclass-declare.tur` — kind-polymorphic typeclass definition
- [ ] `hkt-typeclass-instance.tur` — instance for kinded type constructor
- [ ] `hkt-typeclass-kind-error.tur` — kind mismatch in instance
- [ ] Negative: `hkt-typeclass-reserved.tur` — cannot define `Functor` before H3

### Exit criterion
Kind-polymorphic typeclasses work; constraint propagation catches kind errors; reserved names protected.

---

## Phase H2 — HKT Dispatch Table

**Goal:** Extend the dictionary-passing dispatch to handle kind-polymorphic typeclasses.

### Tasks

#### Dispatch table generalization (`src/elab.c`)
- [ ] Current key: `(class_name, [arg_type, ...])` for kind-`*` classes
- [ ] New key for HKTs: `(class_name, [arg_type, ...], constructor_kind)`
- [ ] Two-level lookup: first find constructor by kind, then find method by types
- [ ] Cache dictionary structs per unique key to avoid redundant codegen

#### Dictionary struct generation (`src/codegen.c`)
- [ ] For `Functor (option int)`, generate dict with `map` field specialized to `option<int> -> int -> option<int>`
- [ ] Dictionary naming: `dict_Functor_option_int` or hashed variant
- [ ] Ensure dictionaries remain static (global singletons) since they contain only function pointers

#### Interaction with existing typeclasses
- [ ] Kind-`*` typeclasses continue to use the fast path (no kind lookup needed)
- [ ] Mixed constraints: function with both `Eq a` and `Functor f` works correctly
- [ ] No overhead for non-HKT code paths

### Fixtures
- [ ] `hkt-dispatch-basic.tur` — simple Functor instance dispatch
- [ ] `hkt-dispatch-nested.tur` — `Functor (option (vec a))` works
- [ ] `hkt-dispatch-mixed.tur` — mixing kind-`*` and kind-`* -> *` constraints
- [ ] Codegen snapshot: dictionary structs for HKT instances

### Exit criterion
HKT dispatch works; no performance regression for kind-`*` code; dictionaries generate correctly.

---

## Phase H3 — Built-in HKT Typeclasses

**Goal:** Ship standard HKT typeclasses: `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable`.

### Tasks

#### Typeclass definitions (`stdlib/typeclass.tur`)
- [ ] `Functor`:
  ```clojure
  (defclass Functor [f : * -> *]
    (map [a : *, b : *] [f : f a, fn : (-> a b)] : (f b)))
  ```
- [ ] `Applicative` (extends `Functor`):
  ```clojure
  (defclass Applicative [f : * -> *] : Functor f
    (pure [a : *] [x : a] : (f a))
    (ap [a : *, b : *] [ff : f (-> a b), fa : f a] : (f b)))
  ```
- [ ] `Monad` (extends `Applicative`):
  ```clojure
  (defclass Monad [m : * -> *] : Applicative m
    (bind [a : *, b : *] [ma : m a, fn : (-> a (m b))] : (m b)))
  ```
- [ ] `Traversable`:
  ```clojure
  (defclass Traversable [t : * -> *] : Functor t
    (traverse [a : *, b : *, f : Applicative] [ta : t a, fn : (-> a (f b))] : (f (t b))))
  ```
- [ ] `Foldable`:
  ```clojure
  (defclass Foldable [t : * -> *]
    (foldl [a : *, b : *] [ta : t a, b0 : b, fn : (-> b a b)] : b)
    (foldr [a : *, b : *] [ta : t a, b0 : b, fn : (-> a b b)] : b))
  ```

#### Instances for stdlib types
- [ ] `Functor` for `option`, `vec`, `slice`, `ref`, `rc`
- [ ] `Applicative` for `option`, `vec`
- [ ] `Monad` for `option`, `vec`
- [ ] `Traversable` for `option`, `vec`
- [ ] `Foldable` for `option`, `vec`

#### Derived instances
- [ ] `Functor (Pair a)` if `Functor a`? No — `Pair a` is not a functor in `a`. Document this limitation.
- [ ] `Functor (option a)` — yes, `option` is a functor
- [ ] `Traversable (option a)` — yes

### Fixtures
- [ ] `hkt-functor-option.tur` — `map` on `option` works
- [ ] `hkt-monad-option.tur` — `bind`/`pure` for `option`
- [ ] `hkt-traversable-vec.tur` — `traverse` for `vec` with `option` applicative
- [ ] `hkt-instances.tur` — all built-in instances compile

### Exit criterion
All built-in HKT typeclasses defined; instances for stdlib types work; fixtures green.

---

## Phase H4 — Kind-Polymorphic Functions

**Goal:** Allow `defn` to be generic over kind variables, enabling truly polymorphic higher-order functions.

### Tasks

#### Function type extensions (`src/elab.c`)
- [ ] Parse kind-variable parameters: `(defn map [^f : * -> *, a : *, b : *] [f : f a, fn : (-> a b)] : f b)`
- [ ] Kind-variable scoping: `f` is scoped to the function
- [ ] Kind constraint propagation: calling `map` requires the caller to provide a type `f` of kind `* -> *`

#### Typeclass constraint interaction
- [ ] `(defn traverse [^t : * -> *, ^f : Applicative] [ta : t a, fn : (-> a (f b))] : f (t b))`
- [ ] Constraint solving: `t` must have `Traversable` instance, `f` must have `Applicative` instance
- [ ] Error if constraints cannot be satisfied

#### Surface syntax sugar
- [ ] Implicit kind inference: `(defn map [f a b] ...)` infers `f : * -> *` from `f a` usage
- [ ] Constraint-only syntax: `(defn do-notation [^m : Monad] ...)` infers kind from `Monad` constraint

#### Codegen for kind-polymorphic functions
- [ ] Dictionary passing: each kind-polymorphic function gets implicit dictionary parameters
- [ ] Monomorphization: for call sites with concrete types, specialize the function (optional optimization)
- [ ] No code bloat for non-polymorphic uses

### Fixtures
- [ ] `hkt-fn-kind-param.tur` — function with explicit kind parameter
- [ ] `hkt-fn-implicit-kind.tur` — kind inferred from usage
- [ ] `hkt-fn-constraints.tur` — kind-polymorphic with typeclass constraints
- [ ] `hkt-fn-call.tur` — calling kind-polymorphic functions
- [ ] Codegen snapshot: dictionary passing for kind-polymorphic functions

### Exit criterion
Kind-polymorphic functions work; constraints propagate correctly; no code bloat.

---

## Phase H5 — Advanced Kinds

**Goal:** Support more complex kind signatures: binary type constructors, kind aliases, and kind synonyms.

### Tasks

#### Binary type constructors
- [ ] Support `* -> * -> *` kinds (e.g., `result : * -> * -> *`)
- [ ] Partial application: `(result int) : * -> *`
- [ ] Full application: `(result int str) : *`
- [ ] Instance definitions: `(definstance Bifunctor result ...)`

#### Kind aliases (`src/types.h`)
- [ ] `(defkind Bifunctor [f : * -> * -> *] f)` — kind synonym
- [ ] Use in typeclass definitions: `(defclass Bifunctor [f : Bifunctor] ...)`
- [ ] Expand aliases during kind checking

#### Kind equality and subtyping
- [ ] Kind equivalence: `* -> * -> *` is equivalent to `* -> (* -> *)`
- [ ] No kind subtyping in v1 (kinds are exact match only)
- [ ] Reserve syntax for future kind subtyping

#### Higher-kinded data types
- [ ] Support `(defstruct Fix [f : * -> *] (unfix : f (Fix f)))` — recursive types
- [ ] Support `(defstruct Free [f : * -> *] (Pure, Roll [a : *] [f (Free f a)]))` — free monad

### Fixtures
- [ ] `hkt-binary-ctor.tur` — binary type constructor kinds
- [ ] `hkt-kind-alias.tur` — kind synonyms work
- [ ] `hkt-recursive-type.tur` — recursive HKT definitions
- [ ] `hkt-free-monad.tur` — free monad encoding

### Exit criterion
Binary type constructors work; kind aliases resolve; recursive HKT types compile.

---

## Phase H6 — Integration & Polish

**Goal:** Production-ready HKT support with documentation, stdlib migration, and performance validation.

### Tasks

#### Documentation
- [ ] `docs/hkt-guide.md` — user-facing guide to HKTs in Turmeric
- [ ] Update `README.md` with HKT examples
- [ ] Add HKT section to language tutorial
- [ ] Document kind system in reference manual

#### Stdlib migration
- [ ] Migrate existing monadic code to use `Monad` typeclass
- [ ] Add `do` notation macro that works with any `Monad`
- [ ] Add `for` comprehension macro using `Monad`/`Traversable`
- [ ] Add `maybe` type as alias for `option` with Monad instance

#### Performance
- [ ] Benchmark dictionary passing overhead for HKT code
- [ ] Benchmark monomorphization optimization
- [ ] Document performance characteristics
- [ ] Add `-O` flag for aggressive monomorphization

#### Testing
- [ ] Property-based tests for typeclass laws (Functor, Monad, etc.)
- [ ] Integration tests: HKTs + closures + defers + refs
- [ ] Negative tests: invalid kind usage, orphan instances

#### Tooling
- [x] `tur explain` support for kind errors (HKT-P5: `--explain TUR-E0012` etc.)
- [ ] IDE integration: kind information in hover tooltips
- [x] Debug dump: `--dump-kinds` flag (HKT-P6)

### Fixtures
- [ ] `hkt-laws-functor.tur` — functor laws: identity and composition
- [ ] `hkt-laws-monad.tur` — monad laws: left identity, right identity, associativity
- [ ] `hkt-integration.tur` — HKTs with closures, defers, refs
- [ ] `hkt-stdlib.tur` — stdlib uses HKT typeclasses

### Exit criterion
All HKT features documented; stdlib migrated; performance benchmarks acceptable; all tests green.

---

## Non-Goals (v1)

The following are explicitly out of scope for the initial HKT implementation:

1. **Higher-ranked types** — No `(forall f. ...)` or `(exists f. ...)`
2. **Type families** / associated types — Deferred to post-HKT
3. **GADTs** — Requires kind equalities, deferred
4. **Kind polymorphism in data types** — `(data Fix f = Fix (f (Fix f)))` deferred
5. **Subkinding** — Kinds are nominal, not structural
6. **Impredicative types** — No kinds that mention themselves

---

## Resolved Questions

1. **Kind inference algorithm:** Use Damas-Milner
   - Rationale: Completeness for kind inference is preferred over simplicity

2. **Dictionary passing vs. monomorphization:** Dictionary passing by default, `-O` flag for monomorphization
   - Keeps code size small by default while allowing optimization when needed

3. **Kind error messages:** Start with simple "expected kind X, got Y", enhance to full unification context based on user feedback

4. **Backward compatibility:** HKTs are strictly additive; kind-`*` typeclasses work unchanged
5. **Orphan instance rule:** Allow orphans
   - Rationale: Enables extending typeclasses later; user prefers flexibility over strict coherence guarantees

---

## Open Questions

None at this time.

---

## Rollout Plan

1. **Feature flag:** `-Xhkt` enables HKT support (off by default)
2. **Experimental release:** Ship behind feature flag in next minor version
3. **Stdlib migration:** Migrate stdlib to use HKTs gradually
4. **Stabilization:** Gather feedback, fix bugs, improve error messages
5. **Default on:** Enable HKTs by default in a major version

---

## Estimated Timeline

| Phase | Duration | Dependencies |
|---|---|---|
| H0 | 1–2 weeks | Phase 15 complete |
| H1 | 2–3 weeks | H0 complete |
| H2 | 1–2 weeks | H1 complete |
| H3 | 2–3 weeks | H2 complete |
| H4 | 3–4 weeks | H3 complete |
| H5 | 2–3 weeks | H4 complete |
| H6 | 1–2 weeks | H5 complete |
| **Total** | **12–19 weeks** | |

---

## References

- [Turmeric Phase 15: Typeclasses](turmeric-plan.archive.md#1016-phase-15--typeclasses)
- [Typeclasses and HKT Design §12.2](turmeric-plan.archive.md#122-type-system)
- [Haskell 98 Report — Kinds](https://www.haskell.org/onlinereport/haskell2010/haskellch11.html)
- [Damas-Milner Type Inference](https://dl.acm.org/doi/10.1145/359576.359587)
- [Rust Traits and Higher-Kinded Types](https://blog.rust-lang.org/2015/05/11/traits.html)
- [Scala Higher-Kinded Types](https://docs.scala-lang.org/tour/higher-kinded-types.html)
