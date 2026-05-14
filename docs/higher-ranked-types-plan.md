# Higher-Ranked Types (HRT) Implementation Plan for Turmeric

> **Status:** HRT0–HRT4 Complete (HRT4 partial — container storage deferred to HRT5); HRT5 pending  
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

## Motivating Examples

This section shows concrete Turmeric code that becomes possible — or becomes dramatically cleaner — with HRT support. Each example is paired with the equivalent without HRTs to illustrate the gap.

### 1. Polymorphic function as an argument (Rank-2)

**Without HRTs** you must pick a concrete type for the callback before passing it:

```clojure
; Works only for int → int
(defn apply-twice-int [f : (-> int int), x : int] : int
  (f (f x)))

(apply-twice-int (fn [n] (+ n 1)) 3)   ; → 5
; (apply-twice-int to-string 3)         ; ERROR — wrong type
```

**With HRTs** the callback is truly polymorphic; the caller decides the type at each use site:

```clojure
(defn apply-twice [f : (forall [a] (-> a a)), x : int] : int
  (f (f x)))

(apply-twice (fn [n] (+ n 1)) 3)       ; → 5   (a = int)
(apply-twice (fn [s] (str s "!")) "hi") ; → "hi!!"  (a = string)
```

> **Why rank-2?** `(forall [a] (-> a a))` appears inside the argument list, not at the top level. Rank-1 inference cannot express "this argument must work for *all* types simultaneously."

---

### 2. `runST` — safe mutable state with phantom regions

A classic HRT application: a stateful computation is parameterised by a phantom region variable `s`. Because `s` never escapes the `run-st` call, the state cell cannot be observed outside the computation.

```clojure
; State-thread token — 's' is phantom, never a real value
(deftype ST [s a] (forall [ignored] (-> (STToken s) a)))

; run-st requires f to work for *any* phantom s
(defn run-st [f : (forall [s] (-> (ST s a) a))] : a
  (f (make-st-token)))

; new-ref and read-ref only work inside the same ST region
(defn new-ref  [init : a] : (ST s (STRef s a)) ...)
(defn read-ref [r : (STRef s a)] : (ST s a) ...)
(defn write-ref [r : (STRef s a), v : a] : (ST s unit) ...)

; Usage — safe: the STRef cannot leak outside run-st
(run-st (fn [_tok]
  (let [r (new-ref 0)]
    (write-ref r 42)
    (read-ref r))))   ; → 42

; Compile-time error — STRef escaping its region:
; (def leaked (run-st (fn [_tok] (new-ref 0))))
```

> **Why this works:** `run-st` receives `f : (forall [s] ...)`. Inside `f`, `s` is a rigid (skolem) variable. Any `STRef s a` produced inside `f` mentions `s`, so the type-checker catches any attempt to return it through `run-st`.

---

### 3. Existential types — abstract data types / modules

Existential types let you hide the concrete representation of a value behind an interface, mimicking ML-style modules.

```clojure
; An "any showable" value — the concrete type 'a' is hidden
(deftype AnyShowable (exists [a] [a (-> a string)]))

; Packing a value together with its show function
(def show-int  : AnyShowable (pack int  [42,      int-to-string]))
(def show-bool : AnyShowable (pack bool [true,     bool-to-string]))

; Consuming without knowing the concrete type
(defn show-it [s : AnyShowable] : string
  (open s [a [val show-fn]]
    (show-fn val)))

(show-it show-int)   ; → "42"
(show-it show-bool)  ; → "true"
```

You can collect heterogeneous showable values in a single list:

```clojure
(def things : (vec AnyShowable)
  [(pack int  [1,     int-to-string])
   (pack bool [false, bool-to-string])
   (pack str  ["yo",  identity])])

(map show-it things)  ; → ["1", "false", "yo"]
```

> **Without existentials** you would need a tagged union or a `Showable` typeclass instance stored alongside the value — a verbose, manually-maintained pattern.

---

### 4. van Laarhoven Lenses (Rank-2 optics)

Lenses are the canonical Rank-2 example from functional programming. A lens focuses on a field `a` inside a structure `s`:

```clojure
; A Lens is a rank-2 function — 'f' must be any Functor
(deftype Lens [s a]
  (forall [f : Functor]
    (-> (-> a (f a)) s (f s))))

; Getter and setter derived from a single Lens value
(defn view [lens : (Lens s a), s : s] : a
  (let [r (lens (fn [a] (Identity a)) s)]
    (run-identity r)))

(defn over [lens : (Lens s a), f : (-> a a), s : s] : s
  (run-identity (lens (fn [a] (Identity (f a))) s)))

(defn set [lens : (Lens s a), v : a, s : s] : s
  (over lens (fn [_] v) s))

; A concrete lens for a struct field
(defn name-lens [f : (-> string (f string)), p : Person] : (f Person)
  (map (fn [n] { p | name: n }) (f (get p :name))))

; Usage
(def p { name: "Alice", age: 30 })
(view name-lens p)              ; → "Alice"
(set  name-lens "Bob" p)        ; → { name: "Bob", age: 30 }
(over name-lens str-upper p)    ; → { name: "ALICE", age: 30 }

; Lens composition is plain function composition — no special operator needed
(def street-lens (comp address-lens street-inner-lens))
```

> **The rank-2 insight:** `(forall [f : Functor] ...)` inside the lens type means the *same* lens value can be used for both reading (with `Identity` functor) and writing (with `Const` functor) — without any runtime dispatch.

---

### 5. Continuation monad (CPS with universal return type)

The continuation type `(Cont r a)` uses a rank-2 `forall` to make the result type polymorphic, enabling `callCC` and delimited continuations:

```clojure
(deftype Cont [a] (forall [r] (-> (-> a r) r)))

(defn return-cont [x : a] : (Cont a)
  (fn [k] (k x)))

(defn bind-cont [c : (Cont a), f : (-> a (Cont b))] : (Cont b)
  (fn [k] (c (fn [a] ((f a) k)))))

(defn run-cont [c : (Cont a), k : (-> a r)] : r
  (c k))

; callCC — capture the current continuation
(defn call-cc [f : (-> (-> a (Cont b)) (Cont a))] : (Cont a)
  (fn [k] ((f (fn [x] (fn [_] (k x)))) k)))

; Usage — early exit from a loop
(run-cont
  (call-cc (fn [exit]
    (bind-cont (return-cont 10) (fn [x]
      (if (> x 5)
        (exit "too big")
        (return-cont (* x 2)))))))
  identity)   ; → "too big"
```

---

### 6. Church / Böhm-Berarducci encodings

Any algebraic data type can be represented as a rank-2 function (Church encoding), allowing data types to be defined purely through functions without any concrete constructors:

```clojure
; Church-encoded option
(deftype ChurchOption [a]
  (forall [r] (-> r (-> a r) r)))

(defn church-none : (ChurchOption a)
  (fn [none _some] none))

(defn church-some [x : a] : (ChurchOption a)
  (fn [_none some] (some x)))

(defn church-option-map [f : (-> a b), opt : (ChurchOption a)] : (ChurchOption b)
  (fn [none some] (opt none (fn [x] (some (f x))))))

(defn church-option-to-option [opt : (ChurchOption a)] : (option a)
  (opt (none) (fn [x] (some x))))

; Church-encoded list
(deftype ChurchList [a]
  (forall [r] (-> r (-> a r r) r)))

(defn church-nil : (ChurchList a)
  (fn [nil _cons] nil))

(defn church-cons [h : a, t : (ChurchList a)] : (ChurchList a)
  (fn [nil cons] (cons h (t nil cons))))

(defn church-foldr [f : (-> a r r), z : r, xs : (ChurchList a)] : r
  (xs z f))

(defn church-map [f : (-> a b), xs : (ChurchList a)] : (ChurchList b)
  (fn [nil cons] (xs nil (fn [h t] (cons (f h) t)))))
```

---

## Usage Tutorial

This tutorial introduces HRTs in Turmeric step by step. It assumes familiarity with Turmeric's basic syntax and typeclass system.

### Step 1 — Understanding rank

Every Turmeric type has a **rank**:

| Example | Rank | Explanation |
|---|---|---|
| `int` | 0 (monotype) | no quantifiers |
| `(-> int int)` | 0 (monotype) | no quantifiers |
| `(forall [a] (-> a a))` | 1 | top-level `forall` |
| `(-> (forall [a] (-> a a)) int)` | 2 | `forall` inside an argument |
| `(-> (-> (forall [a] (-> a a)) int) int)` | 3 | `forall` nested two levels deep |

In standard Hindley-Milner, all `forall` quantifiers are implicitly at rank 1 and are invisible. HRTs let you write rank-2 and above explicitly.

**Rule of thumb:** the rank is the maximum nesting depth of a `forall` on the *left* side of a `->`.

---

### Step 2 — Your first rank-2 function

Enable HRTs with the compiler flag:

```sh
./build/tur build -Xhrt my-file.tur
```

Write a function that accepts a polymorphic argument using `::` for the type annotation:

```clojure
; apply-id accepts any function that is the identity for its argument type
(defn apply-id [f : (forall [a] (-> a a)), x : int, y : string] : [int string]
  [(f x) (f y)])

; Pass in a concrete identity function — type-checker verifies it works for all 'a'
(apply-id (fn [v] v) 42 "hello")   ; → [42, "hello"]
```

If you omit the annotation on `f`, the compiler emits:

```
error: argument type requires rank-2 annotation
  hint: add `: (forall [a] (-> a a))` to the parameter
```

---

### Step 3 — Rank-2 with typeclass constraints

You can constrain the type variable inside `forall`:

```clojure
; f must work for any type that has a Show instance
(defn print-twice [f : (forall [a : Show] (-> a a)), x : int] : unit
  (do
    (println (show (f x)))
    (println (show (f x)))))
```

The `a : Show` syntax inside the `forall` binder is a kind/constraint annotation: "`a` must have a `Show` instance."

---

### Step 4 — Existential types with `pack` and `open`

Existential types hide a concrete type behind an interface. Use `pack` to create an existential value and `open` to consume it:

```clojure
; Define an existential interface — a value plus functions on it
(deftype Counter (exists [s] [s              ; the hidden state type
                               (-> s s)      ; increment
                               (-> s int)])) ; read

; Create a counter backed by an int
(def int-counter : Counter
  (pack int [0
             (fn [n] (+ n 1))
             (fn [n] n)]))

; Create a counter backed by a pair (tracks both steps and parity)
(def pair-counter : Counter
  (pack [int bool]
    [[0 false]
     (fn [[n p]] [(+ n 1) (not p)])
     (fn [[n _]] n)]))

; Consume a Counter without knowing the hidden type
(defn run-counter [c : Counter, steps : int] : int
  (open c [s [state incr read]]
    (let [final (loop [i steps, st state]
                  (if (= i 0) st (recur (- i 1) (incr st))))]
      (read final))))

(run-counter int-counter  5)  ; → 5
(run-counter pair-counter 5)  ; → 5
```

**Key safety rule:** the hidden type `s` inside `open` cannot escape the body. This is checked at compile time:

```clojure
; ERROR — 's' escapes its scope
(defn bad [c : Counter] : ???
  (open c [s [state _ _]]
    state))   ; state has type 's', which is not visible outside 'open'
```

---

### Step 5 — Building a lens

Lenses are the most widely-used rank-2 pattern. Here is a minimal lens library:

```clojure
; Lens type — 'f' ranges over all Functors
(deftype Lens [s a]
  (forall [f : Functor]
    (-> (-> a (f a)) s (f s))))

; Two trivial functors needed to run a lens
(deftype Identity [a] a)
(defn run-identity [x : (Identity a)] : a  x)
(definstance Functor Identity (defn map [f x] (f x)))

(deftype Const [b a] b)
(defn run-const [x : (Const b a)] : b  x)
(definstance Functor (Const b) (defn map [_ x] x))

; Getter: run the lens with Const to extract the focused value
(defn view [lens : (Lens s a), s : s] : a
  (run-const (lens (fn [a] a) s)))

; Setter: run the lens with Identity to rebuild the structure
(defn set [lens : (Lens s a), v : a, s : s] : s
  (run-identity (lens (fn [_] v) s)))

; Modifier
(defn over [lens : (Lens s a), f : (-> a a), s : s] : s
  (run-identity (lens (fn [a] (f a)) s)))
```

Define a lens for a record field with a plain function:

```clojure
(defstruct Point [x : float, y : float])

(defn x-lens [f : (-> float (f float)), p : Point] : (f Point)
  (map (fn [x2] { p | x: x2 }) (f (get p :x))))

(def p { x: 1.0, y: 2.0 })
(view x-lens p)        ; → 1.0
(set  x-lens 5.0 p)   ; → { x: 5.0, y: 2.0 }
(over x-lens (fn [x] (* x 2.0)) p)  ; → { x: 2.0, y: 2.0 }
```

Lens composition is plain function composition — no special operator needed:

```clojure
(defstruct Line [start : Point, end : Point])

(defn start-lens [f, l] (map (fn [s2] { l | start: s2 }) (f (get l :start))))

; Focus on the x-coordinate of the start point of a line
(def start-x-lens (comp start-lens x-lens))

(def ln { start: { x: 0.0, y: 0.0 }, end: { x: 1.0, y: 1.0 } })
(set start-x-lens 3.0 ln)  ; → { start: { x: 3.0, y: 0.0 }, ... }
```

---

### Step 6 — Rank-N and kind quantification

For rank-3 and above you must always provide an annotation. The compiler will tell you if one is missing:

```clojure
; rank-3: the argument itself expects a rank-2 argument
(defn apply-rank2
  [g : (-> (forall [a] (-> a a)) int)] : int
  (g (fn [x] x)))

(defn double-apply
  [h : (-> (-> (forall [a] (-> a a)) int) int)] : int
  (h apply-rank2)

; Combine kind quantification (HKT) with type quantification (HRT)
(defn fmap-any
  [f : (forall [ff : Functor, a, b] (-> (-> a b) (ff a) (ff b)))]
  : ...
  ...)
```

---

### Step 7 — Storing polymorphic values (first-class poly, `-Ximpredicative`)

To store a polymorphic value inside a container you need the additional flag:

```sh
./build/tur build -Xhrt -Ximpredicative my-file.tur
```

```clojure
; A list of identity-like functions, each potentially for a different type
(def polys : (vec (forall [a] (-> a a)))
  [(fn [x] x)
   (fn [x] x)])   ; stored as tur_poly_t entries at runtime

; Retrieve and apply
(let [f (nth polys 0)]
  (f 42))       ; → 42
```

> **Note:** Impredicative types are the most advanced HRT feature. Prefer explicit existential types (`exists`) when possible — they are clearer and do not require `-Ximpredicative`.

---

### Common mistakes and error messages

| Mistake | Error | Fix |
|---|---|---|
| Passing a rank-1 function where rank-2 is expected | `type mismatch: expected (forall [a] ...), got (-> int int)` | Ensure the function is genuinely polymorphic |
| Missing rank-2 annotation on a parameter | `rank-2 type requires explicit annotation` | Add `: (forall [a] ...)` to the `defn` parameter |
| Existential type variable escaping `open` | `existential variable 's' escapes its scope` | Keep all uses of `s` inside the `open` body |
| Using HRT syntax without `-Xhrt` | `unknown type form 'forall' (pass -Xhrt to enable)` | Add `-Xhrt` to the build command |
| Storing a `forall` type in a container without `-Ximpredicative` | `impredicative type requires -Ximpredicative` | Either add the flag or wrap with an existential |

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| HRT0 | `forall`/`exists` syntax and AST | Quantifier annotations parse; no inference yet | Small (0.5–1 week) |
| HRT1 | Rank-2 universal types | Rank-2 functions type-check with bidirectional rules | Medium (2–3 weeks) |
| HRT2 | Existential types | `exists` packing/unpacking works; module-like encapsulation | Medium (2–3 weeks) |
| HRT3 | Rank-N universal types | Arbitrary-rank universal types with annotation-guided inference | Hard (3–5 weeks) | ✓ COMPLETE |
| HRT4 | First-class polymorphic values | Polymorphic values stored in data structures, passed through containers | Hard (3–4 weeks) | ✓ COMPLETE (partial — let/reuse/forwarding; container storage deferred to HRT5) |
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

#### Surface syntax (`src/reader.c`, `src/elab.c`)
- [x] Recognize `forall` as a reserved type-level keyword: `(forall [a] (-> a a))`
- [x] Recognize `exists` as a reserved type-level keyword: `(exists [a] [a (-> a string)])`
- [x] Support multiple bound variables: `(forall [a b] (-> a b a))`
- [ ] Support kind-annotated bound variables: `(forall [f : * -> *] (-> (f int) (f string)))` — deferred to HRT1
- [x] Reject `forall`/`exists` in expression position (type annotations only for now)
- [x] Disambiguate `forall` from user-defined bindings in expression context

#### AST extensions (`src/expr.h`, `src/types.h`)
- [x] Add `TY_FORALL` node: `{ vars: [(name, kind)], body: Type }`
- [x] Add `TY_EXISTS` node: `{ vars: [(name, kind)], body: Type }`
- [ ] Distinguish bound (`forall`-introduced) from free type variables at the AST level — deferred to HRT1
- [x] Add `rank()` helper that computes the rank of a `Type` node (0 = monotype, 1 = rank-1, etc.)
- [ ] Preserve source location through quantifier nodes for diagnostics — deferred to HRT1

#### Pretty-printing (`src/types.c`)
- [x] Print `TY_FORALL` as `(forall [a ...] T)`
- [x] Print `TY_EXISTS` as `(exists [a ...] T)`
- [ ] In error messages, always print quantifiers explicitly (never elide `forall`) — deferred to HRT1

#### Validation pass
- [x] Scope check: all type variables in `body` that appear in `vars` are bound (via extended type_params)
- [x] No shadowing: warn if a `forall`-bound variable shadows an outer type variable
- [ ] Kind check: bound variables carry valid kinds (default `*` when unannotated) — deferred to HRT1

### Fixtures
- [x] `hrt-syntax-forall.tur` — `forall` parses in type annotation position
- [x] `hrt-syntax-exists.tur` — `exists` parses in type annotation position
- [x] `hrt-syntax-multi.tur` — multiple bound variables in one quantifier
- [x] `hrt-syntax-error.tur` — `forall` in expression position is rejected

### Exit criterion
All syntax fixtures parse; `forall`/`exists` AST nodes printed correctly in diagnostics; scope and kind checks pass; no runtime codegen yet.

---

## Phase HRT1 — Rank-2 Universal Types ✓ COMPLETE

**Goal:** Type-check and compile functions whose argument types contain `forall` quantifiers (Rank-2). This is the most practically useful case and the foundation for `runST`-style patterns.

### Tasks

#### Bidirectional type checker (`src/elab.c`)
- [x] Rank-2 application rule: `(forall [a] (-> a a))` params type-checked via `elab_poly_call`
- [x] Propagate expected types through `let` (type instantiation in poly call results)
- [x] Reject non-function passed to rank-2 param: "expected ptr<void>, got int" diagnostic
- [x] `(:: expr type)` ascription via `EX_ASCRIBE` — type erased at codegen

#### Type annotation form (`src/elab.c`, `src/reader.c`)
- [x] `(:: expr type)` inline type ascription form: `::` parsed as symbol in reader
- [x] `defn` signatures carry `forall`-annotated argument types: `[f (forall [a] (-> a a)) x :int]`
- [x] `(-> T1 T2 ... Tn)` function type expression syntax for type annotations
- [x] `EX_ASCRIBE` kind added to expr.h; erased (delegates to inner) at codegen

#### Codegen for rank-2 (`src/emit.c`)
- [x] `tur_poly_fn_t` typedef: `struct { void *env; int64_t (*fn)(void *, int64_t); }` in preamble
- [x] `EX_POLY_WRAP` emits `(tur_poly_fn_t){ NULL, __poly_N }` struct literal at call sites
- [x] Poly wrapper thunks `__poly_N(void *env, int64_t x0) { return inner_fn(x0); }` registered as file-level defs
- [x] `emit_fn_def` and forward declarations use `tur_poly_fn_t` for `is_poly_fn` params
- [x] `is_poly_call` emits `f.fn(f.env, (int64_t)(arg0), ...)` for poly calls
- [x] `EX_ASCRIBE` in `emit_value`/`emit_stmt`: delegates to inner expression

### Fixtures
- [x] `hrt-rank2-identity` — `(forall [a] (-> a a))` argument accepted; identity applied to int
- [x] `hrt-rank2-apply` — poly fn applied to two args, results summed
- [x] `hrt-rank2-annotation` — `::` ascription passes fn as rank-2 arg
- [x] `errors/hrt-rank2-error` — non-function passed to rank-2 param caught

### Exit criterion
Rank-2 functions type-check; codegen produces correct C with `tur_poly_fn_t`; wrapper thunks generated; all HRT1 fixtures green.

---

## Phase HRT2 — Existential Types ✓ COMPLETE

**Goal:** Support `exists` (existential types) for encapsulation, abstract data types, and module-like interfaces. Existentials are dual to universals and enable hiding of implementation-specific type details.

### Tasks

#### Existential type packing (`src/elab.c`)
- [x] Rule `∃-intro` (packing): `(pack value (exists [a] T))` creates an existential value
  ```clojure
  (pack 42 (exists [a] a))
  ```
- [x] Elaborate `pack` form: parse `(exists [a] T)` type annotation, elaborate value
- [x] Runtime representation: scalar values reinterpreted as `void *` via `intptr_t`; pointer values passed directly as `tur_exists_t`

#### Existential type unpacking (`src/elab.c`)
- [x] Rule `∃-elim` (unpacking): `(open packed [a v] body)` unboxes existential, binds `v` to the inner value
  ```clojure
  (open packed-val [a v]
    (println v))
  ```
- [x] Accept `TY_EXISTS` (full type info) or `TY_PTR_VOID` (opaque pointer) as packed expression type
- [x] Scope-bound `v` binding: `v` is only available inside `open` body

#### Abstract data types / module pattern
- [x] Demonstrate module pattern: counter with hidden state (`hrt-exists-module`)
- [x] Demonstrate ADT pattern: `make-showable` / `show-val` (`hrt-exists-adt`)

#### Codegen for existentials (`src/emit.c`)
- [x] `tur_exists_t` typedef added to preamble (`typedef void * tur_exists_t;`)
- [x] `pack` emits: `(tur_exists_t)(intptr_t)((int64_t)(val))` for scalars; `(tur_exists_t)(val)` for pointers
- [x] `open` emits: block scoped `int64_t v = (int64_t)(intptr_t)(packed)` for scalar existentials

### Fixtures
- [x] `hrt-exists-pack` — pack an int as existential, compile check
- [x] `hrt-exists-open` — pack 99, open and println v (prints 99)
- [x] `errors/hrt-exists-error` — open on plain int → error diagnostic
- [x] `hrt-exists-adt` — ADT pattern: make-showable / show-val (prints 77)
- [x] `hrt-exists-module` — counter module with hidden state (prints 3)

### Exit criterion
Existential types pack/unpack correctly; scope restriction enforced; module pattern compiles and runs; all existential fixtures green. ✓

---

## Phase HRT3 — Rank-N Universal Types ✓ COMPLETE

**Goal:** Generalize from Rank-2 to arbitrary-rank universal types. Full type inference is undecidable; the approach is annotation-guided bidirectional checking extended to all ranks.

### Implementation notes

- **Rank-3 calling convention:** `tur_poly_fn_t` (16-byte struct) cannot be passed as `int64_t` through the poly dispatch layer. Solution: pass by pointer via C99 compound literal address `(int64_t)(intptr_t)(&(tur_poly_fn_t){NULL, fn})`; receiving wrapper thunk derefs with `*(tur_poly_fn_t*)(intptr_t)(x)`.
- **`poly_arg_mask`:** New `uint32_t` bitmask on `call_` struct; bit `i` set when arg `i` is a nested poly fn. In poly_call path: pass by pointer. In direct call path: dereference from `int64_t`.
- **Typeclass rank-N:** Methods with `(forall [a] ...)` param types use `tur_poly_fn_t` in the dictionary struct FP type. Call sites with poly fn params bypass dictionary dispatch and call the impl binding directly.
- **`arg_full_types`:** `Type.fn.arg_full_types` array stores the full `TY_FORALL` type for poly fn params; used in `make_poly_wrapper` to detect nested poly fn args.

### Tasks

#### Rank-N checker (`src/elab.c`)
- [x] Generalize bidirectional rules to rank-3 (arbitrary nesting via recursive `make_poly_wrapper`)
- [x] Detect and report when a non-function is passed as a rank-N poly fn arg
- [ ] Track current **polarity** (checking vs. inferring) as types are traversed (deferred to HRT4)
- [ ] Allow `^rank-n` pragma on `defn` to enable rank-N checking for a specific function (deferred)

#### Annotation propagation
- [x] Rank-N annotations on `defn` parameters propagate to call sites via poly wrappers
- [x] `let`-bound poly fn params work through normal binding lookup
- [ ] `if`/`cond` branches share the propagated expected type (deferred to HRT4)

#### Rank-N in typeclasses
- [x] Typeclass methods accept `(forall [a] ...)` parameter types in their signatures
- [x] Dictionary struct FP type uses `tur_poly_fn_t` for poly fn params
- [x] Call sites with rank-N typeclass methods bypass dictionary and call impl directly

#### Interaction with HKT
- [x] Rank-N poly fns work with typeclasses (basic HKT interaction via typeclass dispatch)
- [ ] `(forall [f : * -> *] ...)` kind-annotated forall (deferred; `:` parses as keyword prefix)

### Fixtures
- [x] `hrt-rankn-rank3` — rank-3 function with explicit annotation
- [x] `hrt-rankn-typeclass` — typeclass method with rank-N poly fn parameter
- [x] `hrt-rankn-hkt` — rank-N poly fn inside a typeclass instance
- [x] `hrt-rankn-propagation` — annotation propagation through `let`
- [x] `errors/hrt-rankn-missing` — passing int to poly fn param gives clear diagnostic

### Exit criterion
✓ Arbitrary-rank types type-check with annotation guidance; rank-N typeclass methods work; HKT+HRT fixture green; no crashes on deeply nested quantifiers.

---

## Phase HRT4 — First-Class Polymorphic Values ✓ COMPLETE (partial)

**Goal:** Allow polymorphic values to be passed through functions as first-class values — let-binding, forwarding between functions, and reuse within a function body.

### Implementation notes

- **`source_binding` on Binding**: tracks the original global function binding for let-bound function aliases. `poly_arg_fn_binding` follows this chain so `(let [f id] (apply-poly f 42))` correctly resolves `f` → `id`.
- **`is_poly_fn` propagation through `let`**: when `(let [g f] ...)` and `f` is `is_poly_fn=true`, `g` inherits `is_poly_fn=true` and `poly_type`. The C declaration uses `tur_poly_fn_t g = f`.
- **EX_POLY_WRAP pass-through**: when `wrapper_binding == NULL`, emits the inner expression directly (already a `tur_poly_fn_t`). Used when an `is_poly_fn` binding is passed as a poly fn arg — no wrapper thunk needed.
- All three changes apply to `elab_call_fn`, `elab_poly_call`, and `elab_method_call` consistently.

### Tasks

#### Poly fn let-binding (`src/elab.c`, `src/emit.c`)
- [x] Let-bound alias of global function used as poly fn arg
- [x] Let-bound alias of `is_poly_fn` param inherits poly fn metadata
- [x] Multi-level let alias chains (`(let [g f] ...)` where `f` is already let-bound) work
- [x] `tur_poly_fn_t` C declaration for `is_poly_fn` let-bindings

#### Poly fn forwarding (`src/elab.c`)
- [x] `is_poly_fn` binding passed as poly fn arg to another function (pass-through EX_POLY_WRAP)
- [x] Poly fn param forwarded to another rank-2 function call within same body
- [x] Poly fn param reused multiple times in same function body

#### Deferred (HRT5)
- [ ] `(vec (forall [a] (-> a a)))` — container storage of poly fns (requires `tur_poly_t` fat pointer)
- [ ] `(option (forall [a] (-> a a)))` — poly fn in option container
- [ ] Returning a poly fn from a function (return type as forall)
- [ ] Anonymous closure `(fn [x] x)` as rank-2 arg (requires impredicativity flag)
- [ ] `-Ximpredicative` flag for opting into full impredicative use

### Fixtures
- [x] `hrt-impred-let` — let-bound alias of global function passed as poly fn
- [x] `hrt-impred-reuse` — poly fn param forwarded to another rank-2 function
- [x] `hrt-impred-closure` — poly fn used in multiple positions (let alias, conditional, repeated calls)
- [x] `errors/hrt-impred-error` — non-function let-bound value rejected with clear diagnostic

### Exit criterion
✓ Let-bound poly fn aliases work; poly fn params forward correctly; `is_poly_fn` propagates through let-bindings; all 22 HRT fixtures green.

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
