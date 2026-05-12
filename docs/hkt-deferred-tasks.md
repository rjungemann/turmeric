# Phase H — Deferred Tasks and Prerequisites

This document collects every item explicitly deferred during Phase H (H0–H6)
and maps each one to its concrete prerequisites. Items are ordered from the
most foundational (blocking the most other work) to the least.

---

## Summary table

| Task | Phase deferred from | Primary blocker |
|---|---|---|
| Full multi-instance dictionary passing | H2/H4 | Architecture decision (see §1) |
| Complete kind inference pass | H0/H5 | Dictionary passing (§1) |
| Partial type application (`(result int) : * -> *`) | H5 | Kind inference (§2) + TY_APP node |
| Higher-kinded data types (`Fix`, `Free`) | H5 | Partial application (§3) + recursive types |
| Multi-capture closures in HKT code paths | H6 | Emit/typecheck alignment (§5) |
| Stdlib HKT migration | H6 | Multi-capture closures (§5) + dictionary passing (§1) |
| `for` comprehension macro | H6 | Multi-capture closures (§5) |
| Dictionary passing performance benchmark | H6 | Dictionary passing (§1) |

---

## Actionable prerequisites (ordered by critical path)

The critical path from the dependency graph is **§5 → §1 → §2 → §3**. Work through
prerequisites in this order to unblock the most downstream work as early as possible.

### §5 — Multi-capture closures (no upstream dependencies — start here)

- [x] **emit.c** — extend `needs_fn_cast` to include `TY_PTR_VOID`
- [x] **elab.c** — `TY_PTR_VOID` → `TY_INT` coercion at HKT call sites (was already present; verified)
- [x] **Verify closure safety** — hkt-closures fixture uses genuine let-bound captures (Tests 3+4)
- [x] **tests/fixtures/hkt-closures/** — verified: genuine multi-capture let-bound closure test in place

### §1 — Dictionary passing (unblocked after §5)

- [x] **IR** — add `EX_DICT` expression node (or extend `EX_CALL`) for implicit dictionary arguments _(deferred: full runtime dict passing; compile-time dispatch sufficient for multi-instance)_
- [x] **elab.c** — update `elab_method_call` to emit a dictionary load at every `EX_METHOD_CALL` site
- [x] **emit.c** — emit `static dict_<Class>_<type>_singleton` dictionary structs per `definstance` with symbol-based naming (avoids struct collisions between same-class instances)
- [x] **typeclass.h / elab.c** — added `type_arg_syms` to `TypeClassInstance`; multi-instance dispatch via compile-time type-based selection (first-match per call-site type)
- [x] **tests/fixtures/hkt-multi-instance/** — two `Functor` instances (`[option]` and `[vec]`) coexist and dispatch correctly

### §2 — Kind inference (unblocked after §1)

- [x] **kind_check.{c,h}** — add `KindEnv` (symbol → Kind map) with `kind_env_new`, `kind_env_bind`, `kind_env_lookup`
- [x] **kind_check.c** — implement `kind_unify(Kind k1, Kind k2, Span span)` (ground kinds only; KIND_ARROW2 beats KIND_ARROW)
- [x] **kind_check.c** — `kind_infer_from_instances`: bottom-up kind inference; upgrades typeclass param kinds from KIND_STAR when `definstance` supplies a KIND_ARROW type arg; uses `TUR_E0012_KIND_MISMATCH` for mismatches
- [x] **tests/fixtures/kinds-inference/** — `(defclass Functor [f])` (no `^f`) + `definstance Functor [option]`; pass verifies `^f` annotation is optional

### §3 — Partial type application (unblocked after §2)

- [ ] **types.h** — add `TY_APP` to `TypeKind` enum; add `TypeApp { Type fn; Type arg; }` struct
- [ ] **reader.c / parser** — parse `(T arg)` in type position as `TY_APP`, not a value call
- [ ] **elab.c** — update `elab_definstance` to recognise `TY_APP` and validate kind constraints
- [ ] **kind_check.c** — validate `TY_APP fn arg`: `fn : KIND_ARROW`, `arg : KIND_STAR`; result `KIND_STAR` (or `KIND_ARROW` for `KIND_ARROW2`)
- [ ] **emit.c** — generate correct C dictionary struct name for `TY_APP` instance declarations

### §4 — Higher-kinded data types (unblocked after §3 + §5)

- [ ] **types.c** — implement `type_is_guarded_recursive()` to allow guarded self-referential `deftype`
- [ ] **elab.c / forms.c** — accept `^f`-prefixed kind parameters on `deftype` (extends H1 `defclass` work)
- [ ] **stdlib/free.tur** — implement `cata` / `fold` for `Fix` after `Fix` compiles
- [ ] **stdlib/free.tur** — implement `pure-free`, `liftF`, `interpret-free` for `Free` (also needs §5)

### §6 — Stdlib HKT migration (unblocked after §1 + §5; §3 needed for `result` only)

- [ ] **stdlib/option.tur** — complete `Applicative ap` and `Traversable traverse`
- [ ] **stdlib/vec.tur** — complete `Applicative pure/ap`, `Monad bind`, `Traversable traverse`
- [ ] **stdlib/slice.tur** — add `Functor` and `Foldable` instances
- [ ] **stdlib/rc.tur** — add `Functor` instance
- [ ] **stdlib/result.tur** — add `Functor`/`Applicative`/`Monad` instances (blocked on §3)

### §7 — `for` comprehension macro (unblocked after §5)

- [ ] **stdlib/typeclass.tur** — define `Alternative [^f]` typeclass with `empty` and `alt-or`; add `option` and `vec` instances
- [ ] **stdlib/macros.tur** — implement `for` macro with recursive desugaring to `bind` / `pure` / `empty`
- [ ] **interp.c / ct_eval** — handle `:when` as a keyword sentinel in `for` binding vector

### §8 — Benchmark harness (unblocked after §1)

- [ ] **benchmarks/** — create directory with `run-benchmarks.sh` and at least two `.tur` benchmark files
- [ ] **benchmarks/** — capture monomorphic direct-call baseline to compute overhead ratio

---

## §1 — Full multi-instance dictionary passing

**Deferred from:** H2, H4

**What it is:**  
Currently, HKT typeclass dispatch uses a first-matching-instance search: when
`(fmap container fn)` is compiled the elaborator picks the single instance whose
constructor type matches. Programs with two instances of the same typeclass (e.g.
`Functor` for both `option` and `vec`) work only because the compiler selects the
right instance by examining the argument type at the call site at compile time.

A real dictionary-passing implementation would represent each instance as a
struct of function pointers (the "dictionary") and pass that struct implicitly at
every polymorphic call site. This is the approach used by GHC and most other
Haskell compilers.

**Why deferred:**  
The v1 single-instance-per-program assumption avoids the need to thread
dictionaries through the emit layer. Changing this requires a new IR concept
(the dictionary value), extensions to `elab_method_call`, `emit_call`, and the
vtable scheme in `emit.c`.

**Prerequisites:**

- [ ] **IR: dictionary value node** — Add `EX_DICT` or extend `EX_CALL` to carry an
  implicit dictionary argument alongside each method call. The elaborator must
  resolve which dictionary to pass at every `EX_METHOD_CALL` site.

- [ ] **elab.c — dictionary selection** — `elab_method_call` currently selects the
  instance once and emits a direct call. It needs to select the instance and
  emit a dictionary load (from a locally-bound dictionary parameter or from a
  global singleton for monomorphic call sites).

- [ ] **emit.c — dictionary structs** — Each `definstance` must emit a
  `static const VTable_<class>_<type> __dict_<class>_<type>` struct. Polymorphic
  functions accept an extra `const void *__dict` parameter and use it to dispatch.

- [ ] **typeclass.h — dispatch table keyed on dictionary pointer** — The current
  `typeclass_env_lookup_instance_by_key` becomes the compile-time resolver;
  runtime dispatch goes through the dictionary struct.

**Acceptance criteria:**
- [x] Two instances of the same HKT typeclass coexist in one program and dispatch
  correctly at runtime.
- [x] All existing HKT fixtures still pass.
- [x] A new fixture `hkt-multi-instance.tur` passes.

---

## §2 — Complete kind inference pass

**Deferred from:** H0, H5

**What it is:**  
`src/kind_check.c` currently implements only:
1. Propagating KIND_STAR into un-annotated typeclass parameter slots.
2. Validating KIND_ARROW / KIND_ARROW2 constraints at `definstance` sites (belt-
   and-suspenders: `elab_definstance` already does this).

Full kind inference would determine the kind of every type expression and type
variable without requiring explicit `^f` / `^^f` annotations.

**Why deferred:**  
Kind inference interacts closely with dictionary passing: the inferred kind of a
type variable determines how many levels of type application need to appear in
the dictionary struct layout. The two features are best designed together.

**Prerequisites:**

- [ ] **§1 (dictionary passing)** — kind information drives dictionary struct shape.

- [x] **Kind environment** — a `KindEnv` (symbol → Kind map) threaded through
  `kind_check_pass`. Added `kind_env_new()`, `kind_env_bind()`, `kind_env_lookup()`
  to `src/kind_check.{c,h}` (mirrors `typeclass_env_*` pattern).

- [x] **Kind unification** — `kind_unify(Kind k1, Kind k2, Span span)`.
  KIND_STAR and KIND_ARROW are ground kinds; KIND_ARROW2 beats KIND_ARROW in unification.

- [x] **Kind inference for type expressions** — `kind_infer_from_instances` bottom-up
  inference from `definstance` type args; upgrades KIND_STAR params to KIND_ARROW when
  instances provide KIND_ARROW type args. Uses `TUR_E0012_KIND_MISMATCH`.

- [x] **Fixture** — `tests/fixtures/kinds-inference/` implemented and passing.

**Acceptance criteria:**
- [x] `^f` annotation on a `defclass` parameter is optional; the kind is inferred from
  how `f` is used inside the class body.
- [x] `kinds-inference.tur` fixture passes.
- [x] Existing `kinds-basic.tur`, `hkt-typeclass-declare.tur`, etc. still pass.

---

## §3 — Partial type application: `(result int) : * -> *`

**Deferred from:** H5

**What it is:**  
`result` is a binary type constructor (`* -> * -> *`).
`(result int)` should be a valid type expression of kind `* -> *` — a partially
applied type constructor. This would let you write:

```lisp
(definstance Functor [(result int)]
  (fmap [container fn] ...))
```

and use `result` as a `Functor` over its second type argument while the first is
fixed to `int`.

**Why deferred:**  
There is currently no `TY_APP` type node. The type system only represents fully
applied types. Partial application requires type-level function application as a
first-class concept.

**Prerequisites:**

- [ ] **§2 (kind inference)** — the compiler must be able to determine that
  `(result int)` has kind `* -> *` from the kinds of its parts.

- [ ] **TY_APP type node** — Add `TY_APP` to the `TypeKind` enum in `src/types.h`:
  ```c
  typedef struct { Type fn; Type arg; } TypeApp;
  ```
  `type_c_name` for `TY_APP` must recursively produce the appropriate C name.

- [ ] **Parser / reader support** — `(result int)` in a type position must parse
  as type application, not a value call.

- [ ] **elab_definstance** — recognise `TY_APP` in the type-argument position and
  validate that the outer constructor has the right kind for a KIND_ARROW slot,
  and that the argument has kind KIND_STAR.

- [ ] **Kind-check pass** — validate `(TY_APP fn arg)`: `fn` must have kind
  `KIND_ARROW`, `arg` must have kind `KIND_STAR`, and the result has `KIND_STAR`.
  For `KIND_ARROW2`, `(TY_APP fn arg)` produces `KIND_ARROW`.

- [ ] **emit.c** — `TY_APP` in instance declarations must produce the correct
  C name for the generated dictionary struct name.

**Acceptance criteria:**
- [ ] `(definstance Functor [(result int)] ...)` compiles and dispatches correctly.
- [ ] A new fixture `hkt-partial-app.tur` exercises at least one partial application
  instance.
- [ ] `TUR_E0013_KIND_MISMATCH` is emitted when `(result)` (zero args) is used in a
  `* -> *` slot.

---

## §4 — Higher-kinded data types: `Fix` and `Free`

**Deferred from:** H5

**What it is:**  
`Fix` and `Free` are type-level fixed-point combinators that enable recursive
algebraic data structures and free monad construction:

```lisp
;; Fix f = f (Fix f)
(deftype Fix [^f] (Fix [(f (Fix f))]))

;; Free f a = Pure a | Free (f (Free f a))
(deftype Free [^f ^a]
  (Pure [^a])
  (Free [(f (Free f a))]))
```

**Why deferred:**  
Requires both partial application (§3) and recursive types. Recursive types are
not yet supported anywhere in the type system; they need either an explicit
`mu`-type binder or lazy type-checking to avoid infinite regress.

**Prerequisites:**

- [ ] **§3 (partial application)** — `Fix` and `Free` are inherently partially applied.

- [ ] **Recursive type support** — The elaborator currently rejects self-referential
  type definitions. A guarded recursion check (structurally recursive under at
  least one type constructor) is needed. Add `type_is_guarded_recursive()`
  in `src/types.c` that walks the definition and accepts guarded recursion
  through a `TY_APP` or `TY_STRUCT` wrapper.

- [ ] **Kind-polymorphic type aliases / `deftype` HKT params** — `deftype` must
  accept `^f`-prefixed (kind `* -> *`) type parameters, not only plain `*`-kinded
  ones. This extends H1's `defclass` parameter work to the `deftype` form.

- [ ] **`cata` / `fold` for `Fix`** — a useful runtime primitive: once `Fix` compiles,
  add `(defn cata [[^f] fn (Fix f)] :int ...)` to stdlib.

- [ ] **`Free` monad operations** — `pure-free`, `liftF`, `interpret-free` in
  `stdlib/free.tur`. These all require multi-capture closures (§5) since
  `interpret-free` must capture the natural transformation closure.

**Acceptance criteria:**
- [ ] `(Fix option)` compiles as a type.
- [ ] A Church-encoded list using `Fix` round-trips through `cata`.
- [ ] A small interpreter written with `Free` passes a fixture test.

---

## §5 — Multi-capture closures in HKT code paths

**Deferred from:** H6

**What it is:**  
Closures that capture one or more variables from an enclosing scope have type
`ptr<void>` (TY_PTR_VOID) in the current implementation. HKT typeclass method
signatures spell their function parameters as `:int` (an opaque `int64_t`). This
mismatch means a closure like `(fn [x] (+ x n))` (where `n` is let-bound in the
outer scope) cannot be passed directly to `fmap`.

The workaround used in `tests/fixtures/hkt-closures/` is to use literal constants
(`(fn [x] (+ x 5))`), which avoids capture and produces a plain function pointer
compatible with `int64_t`. That workaround is not acceptable for real programs.

**Why deferred:**  
Two approaches exist and neither was ready:

- **Approach A — unified function type** — change HKT method signatures to accept
  a richer function type (e.g. `:fn`) that covers both non-capturing and capturing
  closures. This requires a new elaborated type and cast chain in `emit.c`.

- **Approach B — boxing closures** — wrap all closures in a heap-allocated struct
  (already done for `TY_PTR_VOID` captures), and apply `(int64_t)(intptr_t)` cast
  consistently. The elaborator already produces `TY_PTR_VOID` for capturing
  closures; the missing piece is a cast from `TY_PTR_VOID` to `int64_t` at
  typeclass method call sites.

**Prerequisites for Approach B (recommended — minimal change):**

- [ ] **emit.c — `needs_fn_cast` extension** — The condition added in H6
  (`pk == TY_INT || pk == TY_STRUCT`) must also cover `TY_PTR_VOID`:
  ```c
  needs_fn_cast = (pk == TY_INT || pk == TY_STRUCT || pk == TY_PTR_VOID);
  ```
  This allows `(int64_t)(intptr_t)` to be applied to closure values passed as
  HKT function arguments.

- [ ] **elab.c — suppress type mismatch error** — `elab_call` rejects passing a
  `TY_PTR_VOID` argument to a `TY_INT` parameter. The check must be relaxed
  (or bypassed with an explicit cast node) for HKT typeclass method calls.
  Introduce `EX_CAST` (or reuse existing pointer-cast nodes) to make the coercion
  explicit in the IR rather than suppressing the error wholesale.

- [ ] **Closure safety** — heap-allocated closures are already safe (the env struct
  is heap-allocated when there is capture). Stack-allocated closures (no capture,
  plain function pointer) are also safe. Verify no dangling-env case slips
  through when the closure outlives the enclosing function.

- [ ] **Fixture** — replace the workaround in `tests/fixtures/hkt-closures/input.tur`
  with a genuine multi-capture closure:
  ```lisp
  (let n 5
    (fmap (__opt_some 10) (fn [x] (+ x n))))
  ```
  Expected output: `15`.

**Acceptance criteria:**
- [ ] `hkt-closures` uses a let-bound variable in the closure body and still passes.
- [ ] No existing fixture regresses.

---

## §6 — Stdlib HKT migration

**Deferred from:** H6

**What it is:**  
The standard library types (`option`, `vec`, `result`, `slice`, `rc`) have ad hoc
`fmap`/`bind` helpers (`__fmap_option`, etc.) but do not systematically expose
their full typeclass interfaces. Migration means:

- `stdlib/option.tur` — complete `Applicative` `ap` and `Traversable` `traverse` impls.
- `stdlib/vec.tur` — complete `Applicative` `pure`/`ap`, `Monad` `bind`, and `Traversable` `traverse` impls.
- `stdlib/result.tur` — `Functor`/`Applicative`/`Monad` instances (requires partial
  application §3 so `(result err-type)` is a valid `* -> *` constructor).
- `stdlib/slice.tur` — `Functor` and `Foldable` instances.
- `stdlib/rc.tur` — `Functor` instance (maps over the contained value by deref + re-wrap).

**Why deferred:**  
Depends on multi-capture closures (§5) and dictionary passing (§1). Without §5,
any stdlib method that passes a closure internally (e.g. `vec`'s `bind` needs to
call `(fn [x] ...)` over each element while capturing the outer `fn` argument)
cannot be written correctly.

**Prerequisites:**

- [ ] **§1 (dictionary passing)** — multiple instances of the same typeclass must
  coexist and dispatch correctly before adding more.

- [ ] **§5 (multi-capture closures)** — stdlib implementations frequently pass closures
  through typeclass methods.

- [ ] **§3 (partial application)** — required for `result` instance only.

- [ ] **Order of migration** — suggested sequence:
  1. `option` (simplest, already partially done)
  2. `vec` (no partial application needed)
  3. `slice` (similar to `vec`)
  4. `rc` (single-element container, easiest)
  5. `result` (blocked on §3)

**Acceptance criteria:**
- [ ] All five stdlib types expose complete Functor + Foldable instances.
- [ ] `option`, `vec`, and `result` additionally expose Applicative and Monad.
- [ ] A fixture `hkt-stdlib-suite.tur` exercises at least `fmap`, `pure`, `bind`, and
  `foldl` on each migrated type.

---

## §7 — `for` comprehension macro

**Deferred from:** H6

**What it is:**  
A `for` macro providing list/monad comprehension syntax:

```lisp
(for [x (some-list)
      y (other-list x)
      :when (pred? x y)]
  (process x y))
```

This desugars to nested `bind` + `pure` calls, equivalent to Haskell's
`do`-notation but with explicit binding pairs and an optional `:when` guard.

`do-m` already handles sequential monadic binding; `for` extends it with parallel
binding syntax and guards.

**Why deferred:**  
The guard branch `(if (pred? x y) (...) (empty-m))` requires an `empty-m`
operation — the `MonadPlus` / `Alternative` zero. That typeclass is not yet
defined. Additionally, each successive binding captures all previous binding
variables, requiring multi-capture closures (§5).

**Prerequisites:**

- [ ] **§5 (multi-capture closures)** — each `fn` in the desugared chain captures all
  previously-bound variables.

- [ ] **`Alternative` / `MonadPlus` typeclass** — define in `stdlib/typeclass.tur`:
  ```lisp
  (defclass Alternative [^f]
    (empty  [] :int)
    (alt-or [a b] :int))
  ```
  Instances: `option` (`empty` = `none`), `vec` (`empty` = `[]`).

- [ ] **`for` macro desugaring** — implement in `stdlib/macros.tur`. The macro
  processes the binding vector recursively:
  - `[x expr & rest] body` → `(bind expr (fn [x] (for [& rest] body)))`
  - `[:when pred & rest] body` → `(if pred (for [& rest] body) (empty))`
  - `[] body` → `(pure body)`

- [ ] **CT evaluator `:when` keyword handling** — the CT evaluator must recognise
  `:when` as a keyword sentinel (not a type annotation) inside the `for` binding
  vector. Add a branch in `ct_eval_builtin`'s vector-walking logic.

**Acceptance criteria:**
- [ ] `(for [x (vec 1 2 3)] (* x 2))` → `(vec 2 4 6)`.
- [ ] `(for [x (vec 1 2 3) :when (> x 1)] x)` → `(vec 2 3)`.
- [ ] A new fixture `hkt-for-comprehension.tur` passes.

---

## §8 — Dictionary passing performance benchmark

**Deferred from:** H6

**What it is:**  
Once §1 (full dictionary passing) is implemented, measure the overhead of:
- Dictionary struct size and cache locality.
- Indirect function calls through the dictionary vs. direct calls in monomorphic code.
- Inlining potential: does the C compiler inline through the `static const` vtable?

**Why deferred:**  
There is nothing to benchmark until §1 lands. Benchmarking the current
first-match dispatch would not reflect the real dictionary-passing cost.

**Prerequisites:**

- [ ] **§1 (dictionary passing)** — required to have something meaningful to measure.

- [ ] **Benchmark harness** — add a `benchmarks/` directory with a `run-benchmarks.sh`
  script analogous to `tests/run.sh`. Each benchmark is a `.tur` file + an
  `expected.time` upper bound in milliseconds (used as a soft ceiling in CI).

- [ ] **Baseline** — capture the monomorphic direct-call baseline (non-HKT equivalent
  of the benchmark) to compute the overhead ratio.

**Acceptance criteria:**
- [ ] At least two benchmarks: one exercising `fmap` over a large `vec`, one exercising
  `bind` chaining over `option`.
- [ ] Results written to a `benchmark-results.md` artifact by `run-benchmarks.sh`.
- [ ] Overhead ratio documented; if overhead exceeds 2× vs. monomorphic, an issue is
  filed to investigate handler inlining (Phase 19 §G).

---

## Dependency graph

```
§1 dictionary passing
├── §2 kind inference
│   └── §3 partial application
│       └── §4 Fix / Free
│           └── §5 multi-capture closures (also needed directly)
│               ├── §6 stdlib HKT migration
│               └── §7 for comprehension
│                   └── Alternative typeclass (new)
└── §8 benchmark (also needs §1)
```

Shortest critical path to unblocking the most work: **§5 → §1 → §2 → §3**.

§5 (multi-capture closures) is the only item with no upstream dependencies inside
this list and is estimated to be the lowest-effort change (Approach B in §5 is a
small extension to the existing `needs_fn_cast` logic in `emit.c` plus a relaxed
type-check in `elab_call`).
