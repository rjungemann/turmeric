---
title: Effects System Guide
category: Advanced Control Flow
description: Algebraic effects, dependency injection, custom control flow
---

# Effects System Guide

This guide explains Turmeric's algebraic effects system and when to use it.

## Overview

Turmeric implements **algebraic effect handlers** inspired by OCaml 5, enabling ergonomic asynchronous programming, custom control flow, and composable abstractions. Effects allow you to perform operations that handlers can intercept and re-route dynamically.

## Core Concepts

### Effects, Performs, and Handlers

Three core primitives:

- **`(defeffect Name [param :type ...] :return-type)`** -- Declares a new effect `Name`.
- **`(perform (Name arg ...))`** -- Raises the effect, searching the dynamic handler stack for a matching handler.
- **`(handle expr (Name [p ...] k) body ...)`** -- Installs a handler. When the effect fires, `k` is the captured continuation; call `(resume k value)` to continue.

```turmeric
;; Declare an effect
(defeffect Read [] :str)

;; Perform the effect
(def name (perform (Read)))

;; Handle the effect
(handle
  (let [name (perform (Read))]
    (println (str "Hello " name)))
  (Read [] k) (resume k "World"))
```
```sweet-exp
;; Declare an effect
defeffect Read [] :str

;; Perform the effect
def name perform(Read())

;; Handle the effect
handle
  let [name perform(Read())]
    println $ str("Hello " name)
  (Read [] k) resume(k "World")
```

### One-Shot Continuations

Continuations in Turmeric are **one-shot**: calling `(resume k v)` consumes `k`, preventing reuse. This matches Turmeric's ownership model. (See [Logic Programming Guide](logic-programming-guide.md) for cloneable continuations.)

### Properties

- **Effects are not in the type system** -- Whether `perform` is handled is checked dynamically. Unhandled effects raise an exception at runtime.
- **Dynamic dispatch** -- The closest matching handler in the call stack handles the effect.
- **Composable** -- Handlers can chain; inner handlers shadow outer ones for the same effect.

### Composing Handlers

Compose handlers for different effects by **nesting `handle` forms** -- the
inner handler runs inside the outer one, and each effect is routed to the
handler that declares it:

```turmeric
(defeffect Log [msg :cstr] :nil)
(defeffect Counter [] :int)

(handle
  (handle
    (do (perform (Log "tick")) (+ (perform (Counter)) 1))
    (Counter [] k) (resume k 41))
  (Log [msg] k) (do (println msg) (resume k nil)))
;; prints "tick", evaluates to 42
```

### First-Class Handler Values

A handler is also a **value**: you can create one, pass it, apply it to a body,
and compose two of them.

- **Create** a handler with the `(handler (E [params] k) body)` literal. It has
  type `(handler E V R)` and carries the case detached from any body.
- **Apply** a handler value with `(with-handler hv body)` -- the body runs with
  `hv`'s dispatch table installed, exactly as if its case were written inline in
  a `handle`. (`with-handler` with case/body pairs instead of a single value
  argument remains the inline-`handle` sugar.)
- **Compose** two handlers over *different* effects with
  `(compose-handlers h1 h2)`; `h1` is the **outer** handler.

```turmeric
(defeffect Log [msg :cstr] :nil)
(defeffect Counter [] :int)

;; compose-handlers builds one handler value over both effects (h1 = Counter is
;; the outer handler).  Applying it is identical to the nested handle above.
(with-handler
  (compose-handlers
    (handler (Counter [] k)   (resume k 41))
    (handler (Log [msg] k)    (do (println msg) (resume k nil))))
  (do (perform (Log "tick")) (+ (perform (Counter)) 1)))
;; prints "tick", evaluates to 42
```

Semantics (precedence, the disjoint-effect rule, answer-type agreement, and
per-case continuation discipline) are specified in
[first-class-handlers-semantics.md](../first-class-handlers-semantics.md).

- **Overlap is rejected.** Composing two handlers for the *same* effect is a
  compile error (`TUR-E0251`); composition is defined only for disjoint effect
  sets.
- **Effects discharge from the row.** `(with-handler hv body)` removes `hv`'s
  handled effect(s) from the body's effect row; a leftover (unhandled) effect
  propagates to the enclosing function just as it does for `handle`.
- **Continuation discipline is per case.** `^linear` / `^multishot` / the
  default affine `k` are enforced inside a handler value identically to inline
  `handle`, and composition never blends two handlers' disciplines.

> *History:* `compose-handlers` was briefly gated (`TUR-E0704`) while handler
> values had no runtime representation. That gate has been removed; handler
> values are now first-class. See
> [first-class-handlers-plan.md](../first-class-handlers-plan.md).

## Common Use Cases

### Direct-Style Async

Effects enable ergonomic async/await (see [Async/Await Guide](async-await-guide.md)):

```turmeric
(async
  (await (read-file "data.txt"))
  (println "done"))
```
```sweet-exp
async
  await $ read-file("data.txt")
  println("done")
```

### Custom Control Flow

Implement generators, early returns, or custom exception handling:

```turmeric
;; Generator: yield values one at a time
(defeffect Yield [v :int] :nil)

(handle
  (do
    (perform (Yield 1))
    (perform (Yield 2)))
  (Yield [v] k) (do (println v) (resume k nil)))
```
```sweet-exp
;; Generator: yield values one at a time
defeffect Yield [v :int] :nil

handle
  do
    perform(Yield(1))
    perform(Yield(2))
  (Yield [v] k)
    do
      println(v)
      resume(k nil)
```

### Dependency Injection

Mock I/O operations in tests:

```turmeric
(defeffect ReadFile [path :cstr] :str)

;; Production
(handle code
  (ReadFile [path] k) (resume k (read-file-real path)))

;; Tests
(handle code
  (ReadFile [path] k) (resume k "mock data"))
```
```sweet-exp
defeffect ReadFile [path :cstr] :str

;; Production
handle code
  (ReadFile [path] k) resume(k read-file-real(path))

;; Tests
handle code
  (ReadFile [path] k) resume(k "mock data")
```

### Transactional Retry

Automatic conflict resolution (see [STM Tutorial](stm-tutorial.md)):

```turmeric
(defeffect Retry [] :nil)

(handle
  (fn []
    (when (< (read-tvar x) 10)
      (perform (Retry))))
  ;; Re-run transaction on conflict
  (Retry [] k) (resume k nil))
```
```sweet-exp
defeffect Retry [] :nil

handle
  fn []
    when {read-tvar(x) < 10}
      perform(Retry())
  ;; Re-run transaction on conflict
  (Retry [] k) resume(k nil)
```

## Effect Rows (Typed Effects)

Effect rows track which effects a function may perform as part of its type. The
compiler infers rows automatically; you can also annotate them explicitly.

### Syntax

An effect row appears between the parameter list and the return type:

```turmeric
;; Annotated: may perform the Write effect
(defn log-msg [msg :cstr] #{Write} :nil
  (perform (Write msg)))

;; Pure: performs no effects
(defn add [a :int b :int] #{} :int
  (+ a b))

;; Row-polymorphic: propagates the row of the function argument
(defn run-twice [f :(fn [] #{e} :int)] #{e} :int
  (+ (f) (f)))
```
```sweet-exp
;; Annotated: may perform the Write effect
defn log-msg [msg :cstr] #{Write} :nil
  perform(Write(msg))

;; Pure: performs no effects
defn add [a :int b :int] #{} :int
  {a + b}

;; Row-polymorphic: propagates the row of the function argument
defn run-twice [f :(fn [] #{e} :int)] #{e} :int
  {f() + f()}
```

The row `#{e}` is a row variable: `run-twice` performs whatever effects `f` performs, no more.

### Compiler flags

| Flag | Effect |
|---|---|
| `--dump-effects` | Print each top-level `defn`'s inferred effect row after checking |
| `--lint-effects` | Warn on unannotated `defn`s whose inferred row is non-empty |
| `--strict-effects` | Under `--strict-effects`, unannotated functions that perform effects get a warning; callers propagate the inferred row |

### Module-level visibility

Effects can be declared `^private` to prevent leakage outside their defining module:

```turmeric
(defeffect ^private InternalLog [msg :cstr] :nil)
```
```sweet-exp
defeffect ^private InternalLog [msg :cstr] :nil
```

A `^private` effect cannot be `perform`ed or `handle`d outside the module that declares it.
Cross-module effect rows are automatically filtered: if a callee internally performs a private
effect, the caller's inferred row does not include it.

To export an effect explicitly:

```turmeric
(defmodule MyLib
  (export (effect Write) (effect Read))
  ...)
```
```sweet-exp
defmodule MyLib
  export (effect Write) (effect Read)
  ...
```

Other modules import it with `:refer [(effect Write)]`.

### Benefits

- **Polymorphism** -- row variables let higher-order functions propagate caller effects.
- **Compile-time checking** -- the compiler verifies that annotated functions do not perform unlisted effects (`TUR-E0009`).
- **Auditing** -- `--dump-effects` shows the full effect signature of every function.

## Integration with Ownership and Defer

Effects interact with Turmeric's `defer` mechanism:

- `defer` cleanup runs correctly even when `perform` is inside the same `do` block (see [Custom Effects Tutorial](custom-effects-tutorial.md) §8).
- Capturing a continuation across a `defer` boundary is handled: the continuation's environment is cleaned up if it crosses a `defer` boundary.

## See Also

- [Async/Await Guide](async-await-guide.md) -- Effects-based async/await syntax
- [Logic Programming Guide](logic-programming-guide.md) -- Backtracking via cloneable continuations
- [STM Tutorial](stm-tutorial.md) -- Composable transactions with effects
- [Custom Effects Tutorial](custom-effects-tutorial.md) -- Step-by-step walkthrough of all effect patterns
- [Effects vs. Monads](effects-vs-monads.md) -- Why effects replace monadic chaining in Turmeric
