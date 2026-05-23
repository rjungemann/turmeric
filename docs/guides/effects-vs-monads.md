---
title: Effects vs. Monads
category: Language Features
description: Design rationale: why effects instead of Haskell-style monads
---

# Effects vs. Monads in Turmeric

Turmeric uses algebraic effects instead of Haskell-style monadic chaining for
most use cases. This guide explains the design decision and shows how each
common monad use case maps to the Turmeric equivalent.

## Why not Haskell-style monads?

The classical Haskell signature:

```haskell
Monad m => m a -> (a -> m b) -> m b
```

requires **higher-kinded types** -- `m` is a type constructor of kind `* -> *`.
Turmeric's typeclass system operates at kind `*` only. Extending to HKTs would
require kind inference, kind-polymorphic dispatch, kind-checking in the
elaborator, and new dispatch-table key shapes -- significant cost for a feature
whose motivation mostly evaporates once effects exist.

The architectural decision:

1. **No HKT typeclasses in v1 of the type system.**
2. **`bind` / `pure` are per-type functions, not a `Monad` typeclass.**
3. **Effects are the primary tool for what people use monads for in Haskell.**

## Where monad use cases land in Turmeric

| Use case | Turmeric answer |
|---|---|
| `IO`, async | Effects (`Io`, `Await`) |
| `State` | Effects (`Get`, `Set`) |
| Exceptions / errors | Effects (`Throw`) or `do-result` macro |
| `Maybe` / optional / short-circuit | Effects (`Fail`) or `do-option` macro |
| `Either` / `Result` | Effects (`Throw`) or `do-result` macro |
| `Logger` / `Writer` | Effects (`Log`) |
| `List` / nondeterminism | Multi-shot effects (planned) |
| Parser combinators | Effects (`Parse-Char`, `Parse-Fail`) -- direct-style |
| Custom domain DSL | Per-type macro + typeclass-resolved `bind` |

## Effect-handler version of "monadic" code

This is what 80% of "monad chaining" turns into.

### Maybe / short-circuit on missing value

```turmeric
(defeffect Fail [] : a)

(defn lookup-port [cfg-key :cstr] :int @ {Fail Read-Config}
  (let [s (perform (Read-Config cfg-key))]
    (cond
      (empty? s) (perform (Fail))
      :else      (parse-int s))))

(handle (lookup-port "http.port")
  (Fail [] _) 8080)   ;; default if anything in the chain fails
```

```sweet-exp
defeffect Fail [] : a

defn lookup-port [cfg-key :cstr] :int @ {Fail Read-Config}
  let [s perform(Read-Config(cfg-key))]
    cond
      empty?(s)  perform(Fail())
      :else      parse-int(s)

handle lookup-port("http.port")
  (Fail [] _)  8080   ;; default if anything in the chain fails
```

No `>>=`, no nested `Just`, no chains of `match`. Direct-style code that fails
through an effect.

### Result / Either with rich errors

```turmeric
(defstruct Cfg-Error [what :cstr where :cstr])
(defeffect Throw [e :Cfg-Error] : a)

(defn read-config [path :cstr] :Config @ {Throw Io}
  (let [text   (read-file path)
        parsed (parse-toml text)]
    (validate parsed)))

(handle (read-config "/etc/foo.toml")
  (Throw [e]  _) (do
                   (eprintln (str-concat "config error: " (.what e)))
                   (default-config))
  (Io    [op] k) (resume k (do-io op)))
```

```sweet-exp
defstruct Cfg-Error [what :cstr where :cstr]
defeffect Throw [e :Cfg-Error] : a

defn read-config [path :cstr] :Config @ {Throw Io}
  let [text   read-file(path)
       parsed parse-toml(text)]
    validate(parsed)

handle read-config("/etc/foo.toml")
  (Throw [e]  _)
    do
      eprintln $ str-concat("config error: " .what(e))
      default-config()
  (Io    [op] k)  resume(k do-io(op))
```

Chains of `bind` threading `Result<T, Error>` become linear, direct-style
code. The handler is the only place errors are visible.

### State threading

```turmeric
(defeffect Get []       :int)
(defeffect Set [v :int] :nil)

(defn counter-step [] :nil @ {Get Set}
  (let [n (perform (Get))]
    (perform (Set (+ n 1)))))

(defn run-with-state [init :int body] :(pair int a)
  (let [s init
        r nil]
    (handle (set! r (body))
      (Get []  k) (resume k s)
      (Set [v] k) (do (set! s v) (resume k nil)))
    (pair s r)))

(run-with-state 0 counter-step)  ; => (pair 1 nil)
```

```sweet-exp
defeffect Get []       :int
defeffect Set [v :int] :nil

defn counter-step [] :nil @ {Get Set}
  let [n perform(Get())]
    perform(Set({n + 1}))

defn run-with-state [init :int body] :(pair int a)
  let [s init
       r nil]
    handle set!(r body())
      (Get []  k)  resume(k s)
      (Set [v] k)
        do
          set!(s v)
          resume(k nil)
    pair(s r)

run-with-state(0 counter-step)  ; => (pair 1 nil)
```

This is the `State` monad as an effect handler. The handler is the
interpretation; callers of `counter-step` don't see the threading.

### Parser combinators

```turmeric
(defeffect Parse-Peek [] :(option char))
(defeffect Parse-Take [] :char)
(defeffect Parse-Fail [] :a)

(defn digit [] :char @ {Parse-Peek Parse-Take Parse-Fail}
  (let [c (perform (Parse-Peek))]
    (cond
      (none? c)     (perform (Parse-Fail))
      (is-digit? c) (perform (Parse-Take))
      :else         (perform (Parse-Fail)))))
```

```sweet-exp
defeffect Parse-Peek [] :(option char)
defeffect Parse-Take [] :char
defeffect Parse-Fail [] :a

defn digit [] :char @ {Parse-Peek Parse-Take Parse-Fail}
  let [c perform(Parse-Peek())]
    cond
      none?(c)      perform(Parse-Fail())
      is-digit?(c)  perform(Parse-Take())
      :else         perform(Parse-Fail())
```

Classical Haskell parser-combinator code looks like `digit >>= \d -> ...`.
The effect version is direct-style -- looks like reading a stream, fails
through `Parse-Fail`. The handler interprets it.

> **Caveat.** Backtracking parsers want multi-shot continuations (resume the
> same `k` more than once with different inputs). v1 effects are one-shot. Until
> multi-shot lands, backtracking parsers either (a) use an explicit input cursor
> with `Parse-Fail` and longest-match semantics, or (b) use a per-type `Parser`
> value with `bind`. See the next section.

## When you actually want a monad value

Some cases want a first-class "this is a value representing a computation":

- Building an AST or query plan to be executed later by another mechanism.
- Cross-thread message passing where the sender constructs a chain and the
  receiver runs it.
- Backtracking / nondeterministic search before multi-shot effects exist.
- Lazy / pull-based streams where the consumer drives evaluation.

For these, write a per-type `bind` and use a `do-monadic` macro:

```turmeric
;; Per-type bind functions -- no Monad typeclass needed.
(defn opt-bind [m :(option a) f :(-> a (option b))] :(option b)
  (cond (some? m) (f (unwrap m)) :else none))

(defn opt-pure [x] :(option a) (some x))

(defn res-bind [m :(result a e) f :(-> a (result b e))] :(result b e)
  (cond (ok? m) (f (unwrap-ok m)) :else m))

(defn res-pure [x] :(result a e) (ok x))
```

```sweet-exp
;; Per-type bind functions -- no Monad typeclass needed.
defn opt-bind [m :(option a) f :(-> a (option b))] :(option b)
  cond
    some?(m)  f $ unwrap(m)
    :else     none

defn opt-pure [x] :(option a)
  some(x)

defn res-bind [m :(result a e) f :(-> a (result b e))] :(result b e)
  cond
    ok?(m)  f $ unwrap-ok(m)
    :else   m

defn res-pure [x] :(result a e)
  ok(x)
```

`bind` and `pure` are just functions per-type, not methods of a `Monad`
typeclass. Call them by name: `(opt-bind ...)`, `(res-pure ...)`.

### `do-monadic` notation

A macro lifts the chaining boilerplate. It takes the `bind` / `pure` names as
parameters, since there is no global "the Monad":

```turmeric
(defmacro do-option [bindings body]
  `(do-monadic opt-bind opt-pure ~bindings ~body))

(defmacro do-result [bindings body]
  `(do-monadic res-bind res-pure ~bindings ~body))
```

```sweet-exp
defmacro do-option [bindings body]
  `(do-monadic opt-bind opt-pure ~bindings ~body)

defmacro do-result [bindings body]
  `(do-monadic res-bind res-pure ~bindings ~body)
```

Usage:

```turmeric
(do-option
  [x (lookup-int "port")
   y (lookup-int "timeout")]
  (opt-pure (+ x y)))
;; => (some 30) if both lookups succeed; none otherwise.
```

```sweet-exp
do-option
  [x lookup-int("port")
   y lookup-int("timeout")]
  opt-pure({x + y})
;; => (some 30) if both lookups succeed; none otherwise.
```

## Tradeoffs vs. Haskell

| Property | Turmeric | Haskell |
|---|---|---|
| `Monad m =>` polymorphism | None -- pick a concrete monad per call site | Full HKT polymorphism |
| `do`-notation | Per-monad macros (`do-option`, `do-result`, ...) | Single `do` polymorphic over `Monad` |
| Most "monad" use cases | Effect handlers (direct-style) | Monad transformers / `mtl` |
| Async / IO | Effects | `IO` monad |
| State | Effects | `State` monad |
| Errors | Effects | `Either` / `ExceptT` |
| Parsers | Effects | Parser combinator monad |
| First-class "computation values" | Per-type bind + macro | Polymorphic `>>=` |
| Multi-shot continuations | Planned (multi-shot effects) | Native via `>>=` for `[]` |

**The deal Turmeric makes:** trade type-level monad polymorphism for
direct-style code via effects. Most "monad-heavy" programs become
effect-using programs that don't look monadic at all. The cases that
genuinely want monad-as-value get a per-type fallback.

## See also

- [effects-system-guide.md](effects-system-guide.md) -- Algebraic effects API
- [custom-effects-tutorial.md](custom-effects-tutorial.md) -- Writing custom effects
- [error-handling-guide.md](error-handling-guide.md) -- Error handling patterns
