# Higher-Ranked Types (HRT) Implementation Plan for Turmeric

> **Status:** Draft — Not Started  
> **Prerequisite:** Phase 15 (Typeclasses) must be complete; HKT phases H0–H1 recommended  
> **Target:** v3 or later  
> **Related:** See [hkt-implementation-plan.md](hkt-implementation-plan.md) §Non-Goals item 1 for the deferral decision

---

## Executive Summary

Higher-Ranked Types (HRTs) extend Hindley-Milner type inference by allowing universal (and existential) quantifiers to appear at positions other than the outermost level of a type signature. In standard Rank-1 HM, every polymorphic function is implicitly `forall`-quantified at the top level; HRTs lift that restriction so that quantifiers can appear inside function argument types (Rank-2), return types, and arbitrarily deep (Rank-N).

**Primary motivators:**

1. `runST`-style region safety — stateful computations polymorphic in a phantom region variable `s`
2. Continuation-passing style with universal continuations — `(forall [r] (-> (-> a r) r))`
3. Encoding of existential types — hiding implementation details behind a type boundary
4. Church / Böhm-Berarducci encodings of algebraic data types
5. Lenses, prisms, and optics expressed as first-class polymorphic values

**Decision rule:** Ship if ≥2 of: (1) library authors need first-class polymorphic callbacks, (2) `runST`-style patterns appear in user code, (3) optics / lens libraries are requested.

**Key constraint:** Full type inference is undecidable for Rank-3 and above (Tiuryn & Urzyczyn 1996). Turmeric's approach is **bidirectional type checking**: the programmer supplies explicit `forall` annotations where needed; inference proceeds everywhere else.

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| HRT0 | `forall`/`exists` syntax and AST | Quantifier annotations parse; no inference yet | Small (0.5–1 week) |
| HRT1 | Rank-2 universal types | Rank-2 functions type-check with bidirectional rules | Medium (2–3 weeks) |
| HRT2 | Existential types | `exists` packing/unpacking works; module-like encapsulation | Medium (2–3 weeks) |
| HRT3 | Rank-N universal types | Arbitrary-rank universal types with annotation-guided inference | Hard (3–5 weeks) |
| HRT4 | First-class polymorphic values | Polymorphic values stored in data structures, passed through containers | Hard (3–4 weeks) |
| HRT5 | Integration & polish | Documentation, stdlib patterns, performance benchmarks | Medium (1–2 weeks) |

---

## Prerequisites Checklist

Before starting Phase HRT0, verify:

- [ ] Phase 15 (Typeclasses, kind-`*`) is complete and stable
- [ ] HKT Phase H0 (kind system) is landed — quantified type variables carry explicit kinds
- [ ] `Type` struct in `src/types.h` can represent polymorphic types (rank-1 `forall` is already implicit; needs explicit node)
- [ ] Elaborator (`src/elab.c`) has a bidirectional checking mode (expected-type threading)
- [ ] Error reporting (`src/diag.c`) can emit type-level diffs with quantifier annotations shown
- [ ] No code currently relies on all type variables being implicitly rank-1

---

## Phase HRT0 — Syntax and AST

**Goal:** Introduce `forall` and `exists` as first-class type-level forms. No inference yet — this phase only parses and represents quantified types in the AST.

### Tasks

#### Surface syntax (`src/reader.c`)
- [ ] Recognize `forall` as a reserved type-level keyword: `(forall [a] (-> a a))`
- [ ] Recognize `exists` as a reserved type-level keyword: `(exists [a] [a (-> a string)])`
- [ ] Support multiple bound variables: `(forall [a b] (-> a b a))`
- [ ] Support kind-annotated bound variables: `(forall [f : * -> *] (-> (f int) (f string)))`
- [ ] Reject `forall`/`exists` in expression position (type annotations only for now)
- [ ] Disambiguate `forall` from user-defined bindings in expression context

#### AST extensions (`src/expr.h`, `src/types.h`)
- [ ] Add `TY_FORALL` node: `{ vars: [(name, kind)], body: Type }`
- [ ] Add `TY_EXISTS` node: `{ vars: [(name, kind)], body: Type }`
- [ ] Distinguish bound (`forall`-introduced) from free type variables at the AST level
- [ ] Add `rank()` helper that computes the rank of a `Type` node (0 = monotype, 1 = rank-1, etc.)
- [ ] Preserve source location through quantifier nodes for diagnostics

#### Pretty-printing (`src/types.c`)
- [ ] Print `TY_FORALL` as `(forall [a ...] T)`
- [ ] Print `TY_EXISTS` as `(exists [a ...] T)`
- [ ] In error messages, always print quantifiers explicitly (never elide `forall`)

#### Validation pass
- [ ] Scope check: all type variables in `body` that appear in `vars` are bound
- [ ] No shadowing: warn if a `forall`-bound variable shadows an outer type variable
- [ ] Kind check: bound variables carry valid kinds (default `*` when unannotated)

### Fixtures
- [ ] `hrt-syntax-forall.tur` — `forall` parses in type annotation position
- [ ] `hrt-syntax-exists.tur` — `exists` parses in type annotation position
- [ ] `hrt-syntax-multi.tur` — multiple bound variables in one quantifier
- [ ] `hrt-syntax-error.tur` — `forall` in expression position is rejected

### Exit criterion
All syntax fixtures parse; `forall`/`exists` AST nodes printed correctly in diagnostics; scope and kind checks pass; no runtime codegen yet.

---

## Phase HRT1 — Rank-2 Universal Types

**Goal:** Type-check and compile functions whose argument types contain `forall` quantifiers (Rank-2). This is the most practically useful case and the foundation for `runST`-style patterns.

### Tasks

#### Bidirectional type checker (`src/elab.c`)
- [ ] Implement **checking mode**: given an expected type, propagate it into subexpressions
- [ ] Implement **inference mode**: infer a type bottom-up (existing HM path)
- [ ] Rule `∀-intro` (checking): when expected type is `(forall [a] T)`, bind `a` as a rigid type variable and check the body against `T`
- [ ] Rule `∀-elim` (inference): when a `forall` type is used at a known concrete type, instantiate the bound variable
- [ ] Rank-2 application rule: when a function expects `(forall [a] (-> a a))`, the argument must be checkable at all monotypes
- [ ] Propagate expected types through `let`, `do`, `if`, `cond` forms
- [ ] Reject higher-rank usage without annotation: emit "rank-2 type requires explicit annotation" diagnostic

#### Type annotation form (`src/forms.c`)
- [ ] Introduce `(:: expr type)` as an inline type ascription form (if not already present)
- [ ] Allow `defn` signatures to carry `forall`-annotated argument types:
  ```clojure
  (defn apply-poly
    [f : (forall [a] (-> a a)), x : int] : int
    (f x))
  ```
- [ ] Validate that ascribed types are well-kinded before elaboration

#### Substitution and unification (`src/types.c`)
- [ ] Distinguish rigid (skolem) type variables from unification metavariables
- [ ] Unification never solves rigid variables: "could not unify rigid `a` with `int`" error
- [ ] Implement `instantiate`: replace `forall`-bound variables with fresh metavariables for inference
- [ ] Implement `skolemize`: replace `forall`-bound variables with rigid skolems for checking
- [ ] Capture-avoiding substitution for quantified types

#### Codegen for rank-2 (`src/emit.c`)
- [ ] A rank-2 argument `(forall [a] (-> a a))` is represented at runtime as a **generic closure** — a pair of `(void *env, void *(*fn)(void *env, void *arg, size_t size))`
- [ ] Size/alignment of `a` is threaded as an implicit parameter (or passed via a small descriptor struct)
- [ ] Emit wrapper thunks when a concrete function is passed where a rank-2 polymorphic function is expected
- [ ] Call sites of rank-2 arguments emit the correct descriptor for the concrete type

### Fixtures
- [ ] `hrt-rank2-identity.tur` — `(forall [a] (-> a a))` argument accepted
- [ ] `hrt-rank2-apply.tur` — function that applies a polymorphic argument to multiple types
- [ ] `hrt-rank2-runst.tur` — `runST`-style phantom region: `(forall [s] (-> (ST s a) a))`
- [ ] `hrt-rank2-annotation.tur` — `::` ascription forces rank-2 checking
- [ ] `hrt-rank2-error.tur` — missing annotation caught; rank mismatch caught

### Exit criterion
Rank-2 functions type-check with bidirectional rules; codegen produces correct C; `runST` pattern compiles and executes; all rank-2 fixtures green.

---

## Phase HRT2 — Existential Types

**Goal:** Support `exists` (existential types) for encapsulation, abstract data types, and module-like interfaces. Existentials are dual to universals and enable hiding of implementation-specific type details.

### Tasks

#### Existential type packing (`src/elab.c`)
- [ ] Rule `∃-intro` (packing): `(pack witness-type value : (exists [a] T))` creates an existential value
  ```clojure
  (pack int 42 : (exists [a] [a (-> a string)]))
  ```
- [ ] Elaborate `pack` form: verify the witness type satisfies `T[a := witness-type]`
- [ ] Runtime representation: existential value is a boxed pair of `(type-descriptor, data-pointer)`

#### Existential type unpacking (`src/elab.c`, `src/forms.c`)
- [ ] Rule `∃-elim` (unpacking): `(open x [a v] body)` binds `a` as a rigid type variable and `v` as the hidden value
  ```clojure
  (open packed-val [a v]
    (let [s : string ((second v) (first v))]
      s))
  ```
- [ ] Enforce scope restriction: `a` must not escape the body of `open`
- [ ] Error: "existential type variable `a` escapes its scope"

#### Abstract data types with existentials
- [ ] Allow `defstruct` to define an existentially-typed field: `(defstruct Showable (exists [a] [a (-> a string)]))`
- [ ] Derive pack/unpack helpers for existentially-typed structs
- [ ] Demonstrate module pattern: struct of functions sharing a hidden state type

#### Codegen for existentials (`src/emit.c`)
- [ ] Emit a tagged union / descriptor struct for each existential type
- [ ] `pack` emits: allocate descriptor, store pointer and size, box the value
- [ ] `open` emits: unbox and bind to local variables, pass type descriptor implicitly

### Fixtures
- [ ] `hrt-exists-pack.tur` — pack an existential value
- [ ] `hrt-exists-open.tur` — open and use an existential value
- [ ] `hrt-exists-escape.tur` — escaped type variable caught at compile time
- [ ] `hrt-exists-adt.tur` — `Showable` abstract data type via existential
- [ ] `hrt-exists-module.tur` — module pattern: record of functions over hidden state

### Exit criterion
Existential types pack/unpack correctly; scope restriction enforced; module pattern compiles and runs; all existential fixtures green.

---

## Phase HRT3 — Rank-N Universal Types

**Goal:** Generalize from Rank-2 to arbitrary-rank universal types. Full type inference is undecidable; the approach is annotation-guided bidirectional checking extended to all ranks.

### Tasks

#### Rank-N checker (`src/elab.c`)
- [ ] Generalize bidirectional rules to arbitrary nesting depth
- [ ] Track current **polarity** (checking vs. inferring) as types are traversed
- [ ] Under a `forall` in checking position: skolemize, recurse into body
- [ ] Under a `forall` in inference position: instantiate with fresh metavariables
- [ ] Detect and report the **minimum rank** needed: "argument requires rank-3 annotation"
- [ ] Allow `^rank-n` pragma on `defn` to enable rank-N checking for a specific function

#### Annotation propagation
- [ ] `defn` return type annotations propagate expected types to the body
- [ ] `let` type annotations thread through to the bound expression
- [ ] `if`/`cond` branches share the propagated expected type
- [ ] `do` blocks: only the last expression gets the propagated type

#### Rank-N in typeclasses
- [ ] Allow typeclass methods to have rank-N types in their signatures
- [ ] Example: `(defclass Category [arr : * -> * -> *] (id [a : *] : (arr a a)) (compose [a b c : *] [(arr b c) (arr a b)] : (arr a c)))`
- [ ] Kind-check rank-N typeclass method signatures
- [ ] Dictionary codegen for rank-N methods: methods with polymorphic arguments emit generic closure fields

#### Interaction with HKT
- [ ] A `forall` can bind a kind variable: `(forall [f : * -> *] (-> (f int) (f string)))`
- [ ] Ensure kind and type quantification compose without conflict
- [ ] Skolem variables carry both a name and a kind

### Fixtures
- [ ] `hrt-rankn-rank3.tur` — rank-3 function with explicit annotation
- [ ] `hrt-rankn-typeclass.tur` — typeclass method with rank-N signature
- [ ] `hrt-rankn-hkt.tur` — combined kind and type quantification
- [ ] `hrt-rankn-propagation.tur` — annotation propagation through `let`/`if`
- [ ] `hrt-rankn-missing.tur` — missing annotation gives clear diagnostic

### Exit criterion
Arbitrary-rank types type-check with annotation guidance; rank-N typeclass methods work; combined HKT+HRT fixtures green; no crashes on deeply nested quantifiers.

---

## Phase HRT4 — First-Class Polymorphic Values

**Goal:** Allow polymorphic values to be stored in data structures, returned from functions, and passed through containers — true impredicative use. This requires a runtime representation for polymorphic values that is independent of any specific type instantiation.

### Tasks

#### Impredicative instantiation (`src/elab.c`)
- [ ] Allow type metavariables to be solved to polymorphic types (e.g., `(vec (forall [a] (-> a a)))`)
- [ ] Guard impredicative use behind a feature flag `-Ximpredicative`
- [ ] Error without the flag: "impredicative type requires `-Ximpredicative`"
- [ ] Implement quick-look impredicativity (Serrano et al. 2020): inspect function arguments before full unification to detect polymorphic pushes

#### Runtime polymorphic value representation
- [ ] Define `tur_poly_t`: a fat pointer pairing `(void *thunk, tur_type_descriptor_t *desc)`
- [ ] A `tur_type_descriptor_t` carries: size, alignment, copy/move/drop function pointers
- [ ] Polymorphic values stored in containers use `tur_poly_t` slots
- [ ] Codegen: when a `forall` type is stored in a `vec` or `option`, box it as `tur_poly_t`

#### Container integration
- [ ] `(vec (forall [a] (-> a a)))` stores `tur_poly_t` entries
- [ ] `(option (forall [a] (-> a a)))` carries a `tur_poly_t` payload
- [ ] Emit appropriate copy/drop logic for `tur_poly_t` in container operations
- [ ] Verify no double-free or leak with ASan in integration tests

#### Interaction with closures
- [ ] A rank-N closure captured in the environment is stored as `tur_poly_t` in the env struct
- [ ] Calling a captured polymorphic closure reconstructs the concrete type from the descriptor at the call site
- [ ] Ensure closure lifetimes are respected (existing `defer` mechanism handles cleanup)

### Fixtures
- [ ] `hrt-impred-vec.tur` — `(vec (forall [a] (-> a a)))` stores and retrieves correctly
- [ ] `hrt-impred-option.tur` — polymorphic value wrapped in `option`
- [ ] `hrt-impred-closure.tur` — rank-N closure captured and called from within a higher-order function
- [ ] `hrt-impred-asan.tur` — no memory errors under ASan
- [ ] `hrt-impred-error.tur` — impredicative use without flag gives diagnostic

### Exit criterion
First-class polymorphic values stored, retrieved, and called correctly; no memory errors; impredicativity flag required and respected.

---

## Phase HRT5 — Integration & Polish

**Goal:** Production-ready HRT support with documentation, stdlib patterns, and performance validation.

### Tasks

#### Documentation
- [ ] `docs/hrt-guide.md` — user-facing guide: when to use HRTs, annotation syntax, common patterns
- [ ] Update `docs/hkt-implementation-plan.md` §Non-Goals to reference this plan
- [ ] Add HRT section to language reference manual
- [ ] Cookbook entries: `runST`, optics, Church encodings, module pattern

#### Standard library patterns (`stdlib/`)
- [ ] `(defn run-st [f : (forall [s] (-> (ST s a) a))] : a ...)` — safe mutable state
- [ ] `(deftype Lens s t a b (forall [f : Functor] (-> (-> a (f b)) s (f t))))` — van Laarhoven lens
- [ ] `(deftype Cont r a (forall [ignored] (-> (-> a r) r)))` — continuation monad
- [ ] `(deftype Church a (forall [r] (-> (-> a r) r r)))` — Church encoding

#### Error message improvements
- [ ] Show rank of inferred vs. expected type in mismatch diagnostics
- [ ] Suggest adding `::` annotation when rank inference fails
- [ ] "Escaped skolem" errors include which `forall`/`open` introduced the variable
- [ ] `tur explain` support for HRT-specific error codes

#### Performance
- [ ] Benchmark overhead of generic closure representation vs. monomorphized paths
- [ ] Measure impact of `tur_poly_t` boxing in container operations
- [ ] Provide `-O` monomorphization path for rank-2 when call site types are known
- [ ] Document performance tradeoffs in `docs/hrt-guide.md`

#### Testing
- [ ] Property tests for quantifier law: `∀a. id @a = id`
- [ ] Integration tests: HRT + closures + defer + typeclasses + HKT
- [ ] Negative tests: rank mismatch, escaped skolems, impredicative without flag
- [ ] Fuzz the type checker with randomly generated rank-N type annotations

### Fixtures
- [ ] `hrt-stdlib-runst.tur` — `run-st` safely encapsulates mutable state
- [ ] `hrt-stdlib-lens.tur` — van Laarhoven lens composes correctly
- [ ] `hrt-stdlib-cont.tur` — continuation monad using HRT
- [ ] `hrt-stdlib-church.tur` — Church-encoded data structure
- [ ] `hrt-integration.tur` — HRT + HKT + typeclasses + closures + defer

### Exit criterion
All stdlib patterns compile and execute; documentation complete; performance benchmarks acceptable; all fixtures green.

---

## Non-Goals

1. **Full Rank-N inference without annotations** — Undecidable in general; out of scope
2. **Dependent types** — Types that depend on values (e.g. length-indexed vectors)
3. **Linear types / uniqueness types** — Separate feature track
4. **GADTs** — Require type equality evidence; deferred post-HRT
5. **Subtyping between ranks** — Rank-1 is not a subtype of Rank-2 in Turmeric's model
6. **Recursive quantification** — `(forall [a] (-> a (forall [a] a)))` allowed, but no `mu`-types

---

## Resolved Questions

1. **Bidirectional vs. full inference:** Bidirectional checking with mandatory annotations at rank-2+ boundaries.
   - Rationale: Full inference is undecidable above rank-2; annotation burden is acceptable for advanced features.

2. **Runtime representation of polymorphic values:** Generic closure (`tur_poly_t` fat pointer) for first-class polymorphic values.
   - Rationale: Avoids specializing the entire call graph; consistent with the dictionary-passing model used for typeclasses.

3. **`pack`/`open` syntax for existentials:** Explicit forms required.
   - Rationale: Makes introduction and elimination of hidden types visible to the reader; avoids type inference ambiguity.

4. **Impredicativity flag:** `-Ximpredicative` required.
   - Rationale: Quick-look impredicativity can be surprising; opt-in matches user expectations.

5. **Interaction with HKT kinds:** `forall`-bound variables carry kinds; kind quantification composes with type quantification.

---

## Open Questions

1. **Syntax for `pack`:** Should `pack` be a keyword or a function? A function form `(pack T val)` is simpler; a keyword allows richer syntax. Needs decision before HRT2.

2. **Should rank-2 be inferred for common patterns?** E.g. `(defn apply-poly [f] (do (f 1) (f "hi")))` forces `f : (forall [a] (-> a unit))` without an annotation. Implementing this requires a constraint-based extension to HM. Assess difficulty vs. ergonomics after HRT1.

3. **Monomorphization at rank-2 call sites:** When the concrete type is known at the call site, can we skip the generic closure and emit a direct call? Evaluate in HRT5.

4. **`tur_type_descriptor_t` ABI:** Exact fields (size, align, copy, drop, hash, eq?) need to be fixed before HRT4 to avoid breaking changes.

---

## Rollout Plan

1. **Feature flag:** `-Xhrt` enables HRT support (off by default)
2. **Rank-2 experimental release:** Ship HRT1 + HRT2 behind `-Xhrt` in a minor version
3. **Rank-N in next minor:** Add HRT3 behind the same flag after rank-2 stabilizes
4. **Impredicativity separately:** `-Ximpredicative` as an additional opt-in
5. **Stabilization:** Gather feedback, improve error messages, harden diagnostics
6. **Default on:** Enable `-Xhrt` by default in a future major version when stable

---

## Estimated Timeline

| Phase | Duration | Dependencies |
|---|---|---|
| HRT0 | 0.5–1 week | Phase 15 complete |
| HRT1 | 2–3 weeks | HRT0 complete; HKT H0 recommended |
| HRT2 | 2–3 weeks | HRT1 complete |
| HRT3 | 3–5 weeks | HRT2 complete |
| HRT4 | 3–4 weeks | HRT3 complete |
| HRT5 | 1–2 weeks | HRT4 complete |
| **Total** | **11.5–18 weeks** | |

---

## References

- [HKT Implementation Plan](hkt-implementation-plan.md) — kind system foundation (prerequisite)
- [Turmeric Phase 15: Typeclasses](turmeric-plan.archive.md) — typeclass system (prerequisite)
- [Practical Type Inference for Arbitrary-Rank Types — Peyton Jones et al. 2007](https://www.microsoft.com/en-us/research/publication/practical-type-inference-for-arbitrary-rank-types/)
- [HMF: Simple Type Inference for First-Class Polymorphism — Leijen 2009](https://www.microsoft.com/en-us/research/publication/hmf-simple-type-inference-for-first-class-polymorphism/)
- [Quick Look Impredicativity — Serrano et al. 2020](https://dl.acm.org/doi/10.1145/3408971)
- [Bidirectional Typing — Dunfield & Krishnaswami 2021](https://arxiv.org/abs/1908.05839)
- [Haskell RankNTypes Extension](https://ghc.gitlab.haskell.org/ghc/doc/users_guide/exts/rank_polymorphism.html)
- [Tiuryn & Urzyczyn — Undecidability of Rank-3 Inference (1996)](https://www.sciencedirect.com/science/article/pii/S089054019690042X)
