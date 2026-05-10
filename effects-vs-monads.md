# Effects vs. Monads in Turmeric

> **Question:** What might monadic chaining look like if the type system and effects system are implemented as planned?

**Short answer:** Once typeclasses (§12.2) and effects (`effects-plan.md`) ship as planned, most monadic-chaining use cases get replaced by effect handlers, and the residual cases use per-type `bind` macros over typeclass-resolved operations. Concrete picture below.

---

## 1. Why classical Haskell-style monads don't fit cleanly

The classical signature

```haskell
Monad m => m a -> (a -> m b) -> m b
```

requires **higher-kinded types** — `m` is a type constructor of kind `* -> *`. §12.2's dictionary-passing typeclasses are planned at kind `*` only. We could extend to HKTs, but the cost is significant (kind inference, kind-polymorphic dispatch, kind-checking in the elaborator, dispatch-table key changes) and the motivation mostly evaporates once effects exist.

So the architectural call is:

1. **No HKT typeclasses in v1 of the type system.**
2. **`bind` / `pure` are per-type, not "the `Monad` typeclass."**
3. **Effects are the primary tool for what people use monads for in Haskell.**

See §6 below for what it would take to lift restriction (1) post-v1.

---

## 2. Where monad use cases land in Turmeric

| Use case | Turmeric answer |
|---|---|
| `IO`, async | Effects (`Io`, `Await`) |
| `State` | Effects (`Get`, `Set`) |
| Exceptions / errors | Effects (`Throw`) or v2 exceptions |
| `Maybe` / optional / short-circuit | Effects (`Fail`) or `do-option` macro |
| `Either` / `Result` | Effects (`Throw`) or `do-result` macro |
| `Logger` / `Writer` | Effects (`Log`) |
| `List` / nondeterminism | Multi-shot effects (v5) — currently no good answer |
| Parser combinators | Effects (`Parse-Char`, `Parse-Fail`) — direct-style |
| Custom domain DSL | Per-type macro + typeclass-resolved `bind` |

---

## 3. The effect-handler version of "monadic" code

This is what 80% of "monad chaining" turns into.

### 3.1 Maybe / short-circuit on missing value

```clojure
(defeffect Fail [] : a)

(defn lookup-port [^cstr cfg-key] : int @ {Fail Read-Config}
  (let [s (perform (Read-Config cfg-key))]
    (cond
      (empty? s) (perform (Fail))
      :else      (parse-int s))))

(handle (lookup-port "http.port")
  (Fail [] _) 8080)              ;; default if anything in the chain fails
```

No `>>=`, no nested `Just`, no chains of `match`. Direct-style code that fails through an effect.

### 3.2 Result / Either with rich errors

```clojure
(defstruct Cfg-Error [^cstr what ^cstr where])
(defeffect Throw [^Cfg-Error e] : a)

(defn read-config [^cstr path] : Config @ {Throw Io}
  (let [text   (read-file path)        ;; perform Io
        parsed (parse-toml text)]      ;; might perform Throw
    (validate parsed)))                ;; might perform Throw

(handle (read-config "/etc/foo.toml")
  (Throw [e]  _) (do
                   (eprintln (concat "config error: " (.what e)))
                   (default-config))
  (Io    [op] k) (resume k (do-io op)))
```

The "monadic-style" version — chains of `bind` threading `Result<T, Error>` — collapses into linear, direct-style code. The handler is the only place errors are visible.

### 3.3 State threading

```clojure
(defeffect Get []      : int)
(defeffect Set [^int v] : nil)

(defn counter-step [] : nil @ {Get Set}
  (let [n (perform (Get))]
    (perform (Set (+ n 1)))))

(defn run-with-state [^int init body] : (pair int a)
  (let [^mut s init
        ^mut r nil]
    (handle (set! r (body))
      (Get []  k) (resume k s)
      (Set [v] k) (do (set! s v) (resume k nil)))
    (pair s r)))

(run-with-state 0 counter-step)   ;; → (pair 1 nil)
```

This is the `State` monad as an effect handler. The handler *is* the interpretation; users of `counter-step` don't see the threading.

### 3.4 Parser combinators (the case where classical Haskell monads shine)

```clojure
(defeffect Parse-Peek [] : (option char))
(defeffect Parse-Take [] : char)
(defeffect Parse-Fail [] : a)

(defn digit [] : char @ {Parse-Peek Parse-Take Parse-Fail}
  (let [c (perform (Parse-Peek))]
    (cond
      (none? c)                              (perform (Parse-Fail))
      (and (>= (unwrap c) \0)
           (<= (unwrap c) \9))               (perform (Parse-Take))
      :else                                  (perform (Parse-Fail)))))

(defn integer [] : int @ {Parse-Peek Parse-Take Parse-Fail}
  (let [^mut acc 0
        ^mut go true]
    (while go
      (let [c (perform (Parse-Peek))]
        (cond
          (and (some? c)
               (>= (unwrap c) \0)
               (<= (unwrap c) \9))
          (let [d (perform (Parse-Take))]
            (set! acc (+ (* acc 10) (- d \0))))
          :else
          (set! go false))))
    acc))
```

Classical Haskell parser-combinator code looks like `digit >>= \d -> ...` or `do { d <- digit; ... }`. The effect version is direct-style — looks like reading a stream, fails through `Parse-Fail`. The handler interprets it (consume input, backtrack, etc.).

> **Caveat.** Backtracking parsers want *multi-shot* continuations (resume the same `k` more than once with different inputs). v1 effects are one-shot. Until multi-shot lands (v5 in `effects-plan.md`), backtracking parsers either (a) use an explicit input cursor + `Parse-Fail` with longest-match semantics, or (b) drop down to a per-type `Parser` value with bind. See §4.

---

## 4. The residual: when you actually want a monad value

Some cases want a first-class "this is a value representing a computation," not an effect to be handled. Examples:

- Building up an AST or query plan that gets *executed later* by some other mechanism.
- Cross-thread message passing where the sender constructs a chain of operations and the receiver runs it.
- Backtracking / nondeterministic search before multi-shot effects exist (see §3.4 caveat).
- Lazy / pull-based streams where the consumer drives evaluation.

For these, you write a per-type `bind` and use a `do-monadic` macro:

```clojure
;; Per-type bind functions — no Monad typeclass needed.
(defn opt-bind [^(option a) m ^(-> a (option b)) f] : (option b)
  (cond (some? m) (f (unwrap m)) :else none))

(defn opt-pure [x] : (option a) (some x))

(defn res-bind [^(result a e) m ^(-> a (result b e)) f] : (result b e)
  (cond (ok? m) (f (unwrap-ok m)) :else m))

(defn res-pure [x] : (result a e) (ok x))
```

`bind` and `pure` are just functions per-type, not methods of a `Monad` typeclass. You call them by name: `(opt-bind …)`, `(res-pure …)`. No HKTs, no instance-resolution gymnastics.

### 4.1 Single-name dispatch via the operator table

If we want a single name `bind` that dispatches on type, the operator dispatch table (§1.1) handles it — but it's single-arg dispatch, not type-constructor dispatch:

```text
;; In the dispatch table:
;; ("bind", [option<a>,    fn], option<b>)    → opt-bind
;; ("bind", [result<a, e>, fn], result<b, e>) → res-bind
```

```clojure
(bind (some 1) (fn [x] (some (* x 2))))  ;; → (some 2), via opt-bind
(bind (ok   1) (fn [x] (ok   (* x 2))))  ;; → (ok 2),   via res-bind
```

The dispatch is on the first argument's type, which is `option<int>` or `result<int, e>`. The table treats `option<…>` and `result<…>` as distinct types — no need to abstract over the type constructor.

This isn't "monad polymorphism" in the Haskell sense (you can't write a function generic over an unknown monad). But for the use cases that survive after effects, you usually know which monad you're in — so dispatching on the concrete type works fine.

### 4.2 `do-monadic` notation

A macro lifts the chaining boilerplate. The interesting part: the macro takes the `bind` / `pure` names as parameters, since there's no global "the Monad" to consult.

```clojure
(defmacro do-monadic [bind-name pure-name bindings body]
  ;; (do-monadic opt-bind opt-pure
  ;;   [x (some 1)
  ;;    y (some 2)]
  ;;   (opt-pure (+ x y)))
  ;; expands to:
  ;; (opt-bind (some 1) (fn [x]
  ;;   (opt-bind (some 2) (fn [y]
  ;;     (opt-pure (+ x y))))))
  ...)

;; Per-monad convenience wrappers:
(defmacro do-option [bindings body]
  `(do-monadic opt-bind opt-pure ~bindings ~body))

(defmacro do-result [bindings body]
  `(do-monadic res-bind res-pure ~bindings ~body))
```

Use:

```clojure
(do-option
  [x (lookup-int "port")
   y (lookup-int "timeout")]
  (opt-pure (+ x y)))

;; (some 30) if both lookups succeed; none otherwise.
```

If the operator dispatch table holds `bind` and `pure` per-type, `do-monadic` can pick them up implicitly:

```clojure
(do-monadic
  [x (lookup-int "port")     ;; → option<int>; bind dispatched to opt-bind
   y (lookup-int "timeout")]
  (pure (+ x y)))            ;; pure dispatched on context — needs type ascription
```

The **`pure` ambiguity** — `pure x` doesn't know which monad to construct — is the same gotcha Haskell has. Solution: type-ascribe the return value, or use the explicit `opt-pure` form.

---

## 5. Tradeoffs vs. Haskell

| Property | Turmeric | Haskell |
|---|---|---|
| `Monad m =>` polymorphism | None — pick a concrete monad per call site | Full HKT polymorphism |
| `do`-notation | Per-monad macros (`do-option`, `do-result`, …) | Single `do` polymorphic over `Monad` |
| Most "monad" use cases | Effect handlers (direct-style) | Monad transformers / `mtl` |
| Async / IO | Effects | `IO` monad |
| State | Effects | `State` monad |
| Errors | Effects | `Either` / `ExceptT` |
| Parsers | Effects | Parser combinator monad |
| First-class "computation values" | Per-type bind + macro | Polymorphic `>>=` |
| Cross-monad code (`runStateT . runExceptT`) | Multiple handlers stacked | Monad transformer stack |
| Multi-shot continuations (List monad, backtracking) | Multi-shot effects (v5) | Native via `>>=` for `[]` |
| Lazy infinite structures (`Stream` monad) | Per-type `Stream` + `bind`; or pull-based effect | Native via lazy `>>=` |

**The deal Turmeric makes:** trade type-level monad polymorphism for direct-style code via effects. Most real "monad-heavy" programs become effect-using programs that don't look monadic at all. The cases that genuinely want monad-as-value get a per-type fallback that's less elegant than Haskell but covers the use cases.

---

## 6. Post-v1: what HKTs would actually cost

If, after v1, real users keep hitting the "I want one function generic over any monad" wall, here's the rough scope of adding HKTs to §12.2's typeclass system. None of this is v1 work; it's a sketch so we know the door isn't bricked shut.

### 6.1 Kind system

- Introduce kinds `*` (types) and `* -> *` (type constructors). Possibly `* -> * -> *` for things like `Either`.
- Kind inference: every type variable in a class head gets a kind; defaults to `*`; promoted to `* -> *` if used in a position like `m a`.
- Kind-checking pass between elaboration and dictionary insertion. Failure mode: "expected kind `* -> *`, got `*`" with a span pointing at the offending instance head.
- `^(option int)` style type ascriptions stay the same — only class definitions and instance heads change.

### 6.2 Dispatch-table changes

- The dispatch table currently keys on `(name, [arg-type, …]) → fn`. HKT dispatch requires keying on the *outer* type constructor (`option`, `result`, …) with the inner types as parameters of the dictionary, not of the lookup key.
- Two-level resolution: look up the constructor's `Monad` dictionary, then call its `bind` slot with concrete `a` and `b`.
- Monomorphization: each `(Monad m, m = option) => …` call site instantiates a specialized `opt-bind` thunk. No runtime dictionary unless the call is genuinely polymorphic across an unknown `m` (which §12.2 today already avoids via specialization).

### 6.3 What it buys

- A single generic `do`-notation macro that doesn't take `bind-name` / `pure-name` arguments.
- Library-level monad transformers (`StateT`, `ExceptT`) become expressible — for users who want the Haskell `mtl` pattern instead of effects.
- `traverse`, `sequence`, `mapM`, `forM`, `replicateM` written once over `Monad m`.
- `Functor` and `Applicative` typeclasses become useful (currently they'd also need HKTs, so they're equally absent in v1).

### 6.4 What it doesn't buy

- Effect-style direct code is still better for `IO` / state / errors / parsers — the effect machinery doesn't go away.
- HKTs don't make multi-shot continuations work; that's still an effects-system problem.
- Doesn't change codegen for any existing program.

### 6.5 Decision rule for post-v1

Add HKTs **only if** at least two of the following are true after a year of real Turmeric use:

1. Users are repeatedly writing per-monad `traverse-option` / `traverse-result` boilerplate that a single `traverse` would eliminate.
2. A library author wants to ship a generic monad transformer or free-monad construction and can't.
3. A meaningful fraction of users come from Haskell / Scala / PureScript and the missing abstraction is the top complaint.

If only (3) is true, the answer is "use effects, that's the language." If (1) and (2) are both true, HKTs pay for themselves.

---

## 7. Other post-v1 ideas this opens up

Adjacent extensions that become interesting once effects + (optional) HKTs exist. None are committed; listed for the design notebook.

- **Effect rows as first-class types.** Today `@ {Throw Io}` is a set on a function type. If rows became first-class (`^(row Throw Io)`), users could write functions parameterized over a row, enabling reusable handler combinators.
- **Effect polymorphism with row variables.** `forall e. (-> a b @ e)` lets a higher-order function be transparent to whatever effects its callback performs. Standard for effect systems (Koka, Eff). Probably wanted before HKTs.
- **`Functor` / `Applicative` without `Monad`.** Even without HKTs, per-type `map` / `ap` via the dispatch table covers most uses. Worth seeding so users don't reach for `bind` when `map` would do.
- **Typed continuation values.** `^(cont a b @ e)` as a first-class type would let users build CPS combinators on top of one-shot effects. Niche but powerful.
- **Algebraic-effect-aware optimizer.** Pure handlers (no `Io`, no `Throw`) can be inlined and fused; the pipeline could specialize handler-passing chains away. Compiler-research territory; defer indefinitely.
- **`do` macro driven by *effect signature*, not by monad.** `(do {Io Throw} body)` could elaborate to whatever the effect runtime needs. Probably unnecessary if direct-style is the default.
- **GADT-ish typed effect operations.** Some effect systems (Koka) want `defeffect` ops whose return type depends on argument values. Possible without full GADTs via existential-style elaboration; revisit if real cases appear.
- **Linear / once handlers.** A handler annotation that statically guarantees `resume` is called exactly once enables more aggressive codegen (no continuation copying). Pairs naturally with §5 ref / ownership.

---

## 8. What this means for the planning docs

Two options for filing this:

1. **Add a `§12.2.x` subsection in `turmeric-plan.md`** under the typeclass discussion, titled "Monadic patterns without HKTs." Locks in the decision that v1 typeclasses are kind-`*` only, points at effects as the substitute for most use cases, and references this doc for the long form.
2. **Keep this as a standalone `effects-vs-monads.md`** (current state) and add a one-line cross-reference from `§12.2` and from `effects-plan.md`.

**Lean: option 2 plus the cross-reference.** The decision (no HKTs in v1) belongs in `turmeric-plan.md` as a single sentence; the rationale and worked examples are long enough to warrant their own file. Just say the word and I'll wire the cross-references in.
