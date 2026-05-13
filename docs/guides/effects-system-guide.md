# Effects System Guide

This guide explains Turmeric's algebraic effects system and when to use it.

## Overview

Turmeric implements **algebraic effect handlers** inspired by OCaml 5, enabling ergonomic asynchronous programming, custom control flow, and composable abstractions. Effects allow you to perform operations that handlers can intercept and re-route dynamically.

## Core Concepts

### Effects, Performs, and Handlers

Three core primitives:

- **`type _ Effect.t += E : T`** — Declares a new effect `E` whose `perform` yields a value of type `T`.
- **`perform E`** — Raises the effect, searching the dynamic handler stack for a matching handler.
- **`try_with body { effc }`** — Installs a handler around `body`. When an effect surfaces, `effc` is called with the current continuation reified as `k`.

```turmeric
;; Declare an effect
type _ Effect.t += Read : string Effect.t

;; Perform the effect
(def name (perform Read))

;; Handle the effect
(try-with
  (fn []
    (let [name (perform Read)]
      (println "Hello " name)))
  (fn [e k]
    (match e
      Read -> (continue k "World"))))
```

### One-Shot Continuations

Continuations in Turmeric are **one-shot**: calling `continue k v` consumes `k`, preventing reuse. This matches Turmeric's ownership model but means **no backtracking in v1**. (See [Logic Programming Guide](logic-programming-guide.md) for cloneable continuations in v2.)

### Properties

- **Effects are not in the type system** — Whether `perform` is handled is checked dynamically. Unhandled effects raise an exception at runtime.
- **Dynamic dispatch** — The closest matching handler in the call stack handles the effect.
- **Composable** — Handlers can chain; inner handlers shadow outer ones for the same effect.

## Common Use Cases

### Direct-Style Async

Effects enable ergonomic async/await (see [Async/Await Guide](async-await-guide.md)):

```turmeric
(async
  (await (read-file "data.txt"))
  (println "done"))
```

### Custom Control Flow

Implement generators, early returns, or custom exception handling:

```turmeric
;; Generator: yield values one at a time
type _ Effect.t += Yield : a Effect.t

(try-with
  (fn []
    (perform (Yield 1))
    (perform (Yield 2)))
  (fn [e k]
    (match e
      (Yield v) -> (println v) (continue k))))
```

### Dependency Injection

Mock I/O operations in tests:

```turmeric
type _ Effect.t += ReadFile : string -> bytes Effect.t

;; Production
(try-with code
  (fn [e k]
    (match e
      (ReadFile path) -> 
        (continue k (read-file-real path)))))

;; Tests
(try-with code
  (fn [e k]
    (match e
      (ReadFile path) -> 
        (continue k "mock data"))))
```

### Transactional Retry

Automatic conflict resolution (see [STM Tutorial](stm-tutorial.md)):

```turmeric
type _ Effect.t += Retry : never Effect.t

(try-with
  (fn []
    (when (< (read-tvar x) 10)
      (perform Retry)))
  (fn [e k]
    ;; Re-run transaction on conflict
    (match e
      Retry -> (continue k))))
```

## Effect Rows (Typed Effects)

A v2 extension puts effects in the type system via **effect rows** — sets of effects a function may perform:

```turmeric
val read_file : path -> string @ (ReadFile)
val pure_fn : int -> int @ ()
val map : ('a -> 'b @ 'e) -> 'a list -> 'b list @ 'e
```

Benefits:
- **Polymorphism** — `map` propagates caller's effects through the mapped function.
- **Compile-time checking** — Verify effect-free code stays pure; catch unhandled effects early.
- **Optimization** — Pure code (`@ ()`) can be aggressively refactored.

**Status:** Planned for v3 of effects design; requires elaborator support and type-system changes.

## Integration with Ownership and Defer

Effects interact with Turmeric's `defer` mechanism (§5, §4 of [turmeric-plan.md](../turmeric-plan.md)):

- Capturing a continuation across a `defer` boundary is the hard case for effects.
- The unified defer model (phase 4) pre-allocates effect-row slots, enabling zero-cost effects in code that doesn't use them.
- On `perform`, the captured continuation's environment is cleaned up if it crosses a `defer` boundary.

## See Also

- [Async/Await Guide](async-await-guide.md) — Effects-based async/await syntax
- [Logic Programming Guide](logic-programming-guide.md) — Backtracking via cloneable continuations
- [STM Tutorial](stm-tutorial.md) — Composable transactions with effects
- [turmeric-plan.md](../turmeric-plan.md) §12 — Delimited continuations (foundation)
