# Custom Effects Tutorial

A step-by-step guide to algebraic effects in Turmeric. Each section builds on the previous one, starting from the simplest possible effect and working up to real-world patterns like mock I/O and dependency injection.

> **Prerequisites**: Working `build/tur` binary (run `make`). No prior knowledge of algebraic effects is required.

---

## Table of Contents

1. [What are algebraic effects?](#1-what-are-algebraic-effects)
2. [Defining and performing an effect](#2-defining-and-performing-an-effect)
3. [Resuming with a value](#3-resuming-with-a-value)
4. [Effects with parameters](#4-effects-with-parameters)
5. [Multiple effects in one handler](#5-multiple-effects-in-one-handler)
6. [Nested handlers](#6-nested-handlers)
7. [Multiple sequential performs](#7-multiple-sequential-performs)
8. [Effects with `defer`](#8-effects-with-defer)
9. [Effects with refs and rc](#9-effects-with-refs-and-rc)
10. [Wrapping a handler in a macro](#10-wrapping-a-handler-in-a-macro)
11. [Bridging effects to exceptions](#11-bridging-effects-to-exceptions)
12. [Checking the continuation with `cont?`](#12-checking-the-continuation-with-cont)
13. [Real-world pattern: mock I/O](#13-real-world-pattern-mock-io)
14. [Real-world pattern: injectable logging](#14-real-world-pattern-injectable-logging)
15. [Summary and next steps](#15-summary-and-next-steps)

---

## 1. What are algebraic effects?

An algebraic effect is a typed, named operation that a function can **perform** without knowing who will handle it. The caller supplies a **handler** that intercepts the operation and decides what to do, including whether to resume the suspended computation with a return value.

Think of it as a structured, type-safe alternative to:
- Global mutable state (replace with an injectable value via `Ask`)
- Printf-style I/O scattered through business logic (replace with a `Write` effect)
- Callbacks (replace with a `perform` that yields control and resumes)

The three primitives are:

| Form | What it does |
|---|---|
| `(defeffect Name [param :type ...] :return-type)` | Declare an effect |
| `(perform (Name arg ...))` | Perform (suspend) the effect |
| `(handle expr (Name [p ...] k) body ...)` | Handle the effect; `k` is the continuation |
| `(resume k value)` | Resume the suspended computation with `value` |

---

## 2. Defining and performing an effect

The simplest effect takes no parameters and returns nothing useful. It is a signal.

```turmeric
;; Declare an effect that signals "I want to emit a line of output."
(defeffect Emit [] :nil)

;; A function that emits twice.
(defn greet [] :nil
  (do
    (perform (Emit))
    (perform (Emit))))

;; Handle by printing a fixed message each time.
(handle (greet)
  (Emit [] k) (do (println "hello!") (resume k nil)))
```

**Output:**
```
hello!
hello!
```turmeric

Key points:
- `defeffect` declares the name, parameter list, and return type.
- `perform` suspends the current function and passes control to the nearest enclosing `handle`.
- The handler receives the continuation `k` and calls `resume k nil` to let the function continue.
- Forgetting to call `resume` terminates the computation at that point.

---

## 3. Resuming with a value

When an effect has a non-`nil` return type, `resume` passes a value back to the call site of `perform`. This is how you inject values into otherwise pure code.

```
;; An effect that "asks" for an integer from the environment.
(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

;; Handler supplies 41; the computation receives 41, adds 1, and returns 42.
(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
; => 42
```

The return type of `perform (Ask)` is `:int` (the declared return type of the `Ask` effect), so the result flows directly into `(+ 1 ...)`.

---

## 4. Effects with parameters

Effects can carry data to the handler. Declare typed parameters just like function arguments.

```turmeric
;; An effect that asks the handler to double a value.
(defeffect Double [x :int] :int)

(defn use-double [n :int] :int
  (perform (Double n)))

(println (handle (use-double 21)
  (Double [x] k) (resume k (* x 2))))
; => 42
```

The handler pattern `(Double [x] k)` binds `x` to the argument supplied by the performer and `k` to the continuation. You can compute any value to pass back via `resume`.

---

## 5. Multiple effects in one handler

A single `handle` block can match several different effects.

```turmeric
(defeffect Ask  []       :int)
(defeffect Tell [x :int] :nil)

;; Performs both effects: asks for a number, then tells the result.
(defn use-both [] :int
  (let [result (+ 1 (perform (Ask)))]
    (do
      (perform (Tell result))
      result)))

(println (handle (use-both)
  (Ask  []    k) (resume k 41)
  (Tell [x]   k) (do (println x) (resume k nil))))
; prints: 42
; returns: 42
```

Handlers are tried in order. Each clause is independent; unhandled effects bubble up to the next enclosing `handle`.

Multiple effects can also carry different result transforms:

```turmeric
(defeffect Add [x :int] :int)
(defeffect Mul [x :int] :int)

(defn compute [] :int
  (* (perform (Add 3)) (perform (Mul 4))))

(println (handle (compute)
  (Add [x] k) (resume k (+ x 10))   ; 3+10 = 13
  (Mul [x] k) (resume k (* x 2))))  ; 4*2  =  8
; => 104
```

---

## 6. Nested handlers

Handlers nest lexically. The innermost matching handler wins.

```turmeric
(defeffect Val [] :int)

(defn get-val [] :int
  (perform (Val)))

;; Outer handler supplies 10; inner overrides with 42 for its scope.
(println (handle
  (+ (get-val)
     (handle (get-val)
       (Val [] k) (resume k 42)))
  (Val [] k) (resume k 10)))
; => 52  (10 + 42)
```

Use this to test functions under different conditions without changing the function itself.

---

## 7. Multiple sequential performs

Each `perform` is an independent suspension. The handler is re-entered for every perform, and each invocation gets its own fresh `k`.

```turmeric
(defeffect Choose [n :int] :int)

;; Calls Choose twice sequentially.
(defn pick-two [] :int
  (+ (perform (Choose 1)) (perform (Choose 2))))

(println (handle (pick-two)
  (Choose [n] k) (resume k (* n 10))))
; First:  1 * 10 = 10
; Second: 2 * 10 = 20
; => 30
```

Continuations are **one-shot**: you must call `resume k` exactly once per handler activation.

---

## 8. Effects with `defer`

`defer` cleanup runs correctly even when `perform` is inside the same `do` block. The continuation is resumed before deferred forms unwind.

```turmeric
(defeffect Ask [] :int)

(defn deferred-ask [] :int
  (do
    (defer (println "cleanup"))
    (perform (Ask))))

(println (handle (deferred-ask)
  (Ask [] k) (resume k 42)))
; prints: cleanup
; prints: 42
```

The `defer` fires when `deferred-ask` returns, which happens after the handler resumes `k` with 42.

---

## 9. Effects with refs and rc

Borrows and reference-counted values that are live at the point of `perform` remain valid across the perform/resume boundary.

```turmeric
;; Refs are live during perform.
(defeffect GetBase [] :int)

(defn sum-with-base [] :int
  (let [base (ref 100)]
    (+ (deref base) (perform (GetBase)))))

(println (handle (sum-with-base)
  (GetBase [] k) (resume k 42)))
; => 142
```

```turmeric
;; RC values are live during perform; no leaks.
(defeffect GetCount [] :int)

(defn use-rc [] :int
  (let [r (rc/of 42)]
    (+ 0 (perform (GetCount)))))

(println (handle (use-rc)
  (GetCount [] k) (resume k 42)))
; => 42
```

The borrow checker enforces that no borrow escapes its declared scope, even through effect boundaries.

---

## 10. Wrapping a handler in a macro

Handlers that are used in many places can be packaged as macros for a cleaner call site.

```turmeric
(defeffect Write [s :cstr] :nil)

;; Package the handler as a macro so callers don't repeat boilerplate.
(defmacro with-write [body]
  (handle body
    (Write [s] k) (do (println s) (resume k nil))))

;; Usage:
(with-write
  (do
    (perform (Write "hello"))
    (perform (Write "world"))))
; hello
; world
```

This is the standard pattern used in `stdlib/effects.tur` for `with-write`, `with-getenv`, and `with-read-console`.

---

## 11. Bridging effects to exceptions

An effect handler can abort the computation (not resume) and throw an exception instead.

```turmeric
(defeffect Fail [msg :cstr] :nil)

(defmacro with-fail-throw [body]
  (handle body
    (Fail [msg] k) (throw! msg)))   ; no resume — aborts the computation

(try
  (with-fail-throw
    (do
      (println "before fail")
      (perform (Fail "something went wrong"))
      (println "after fail")))          ; never reached
  (catch [err :cstr]
    (println err)))
; before fail
; something went wrong
```

Not calling `resume` at all is valid — the computation past the `perform` is simply abandoned. This lets you implement abort-style error signalling over the exception mechanism.

---

## 12. Checking the continuation with `cont?`

`(cont? k)` returns `true` if the continuation has not been consumed. For algebraic effects the static one-shot check guarantees `k` is always unconsumed in the handler body, so this is mostly useful for defensive assertions.

```turmeric
(defeffect Ask [] :int)

(defn ask-with-check [] :int
  (perform (Ask)))

(println (handle (ask-with-check)
  (Ask [] k)
  (do
    (println (cont? k))   ; => true
    (resume k 42))))
; true
; 42
```

---

## 13. Real-world pattern: mock I/O

Replace real I/O with a test double by swapping the handler. The business logic never changes.

```turmeric
(defeffect Read  []        :int)
(defeffect Write [s :cstr] :nil)

;; Pure business logic — no I/O primitives.
(defn echo-doubled [] :int
  (let [n (perform (Read))]
    (do
      (perform (Write (int->cstr (* n 2))))
      0)))

;; Production handler: real stdin/stdout.
(defmacro with-real-io [body]
  (handle body
    (Read  []    k) (resume k (read-int-console))
    (Write [s]   k) (do (println s) (resume k nil))))

;; Test handler: fixed input, captured output.
(defmacro with-test-io [input body]
  (handle body
    (Read  []    k) (resume k input)
    (Write [s]   k) (do (println s) (resume k nil))))

;; In production:
;;   (with-real-io (echo-doubled))
;;
;; In tests:
(with-test-io 21
  (echo-doubled))
; => 42
```

The function `echo-doubled` is completely isolated from I/O. Swapping the handler is the only change needed to go from production to test mode.

---

## 14. Real-world pattern: injectable logging

Define a `Log` effect with levels. Wire it to a real logger in production, suppress it in benchmarks, and capture it in tests.

```turmeric
(defeffect Log [level :cstr msg :cstr] :nil)

;; Business logic
(defn process [x :int] :int
  (do
    (perform (Log "info" "starting"))
    (let [result (* x 2)]
      (do
        (perform (Log "info" "done"))
        result))))

;; Handler: print everything
(defmacro with-stderr-log [body]
  (handle body
    (Log [level msg] k)
      (do (println msg) (resume k nil))))

;; Handler: suppress all logs
(defmacro with-silent-log [body]
  (handle body
    (Log [level msg] k) (resume k nil)))

;; Handler: only print warnings and errors
(defmacro with-warn-log [body]
  (handle body
    (Log [level msg] k)
      (if (or (= level "warn") (= level "error"))
        (do (println msg) (resume k nil))
        (resume k nil))))

(with-stderr-log (println (process 21)))
; starting
; done
; 42
```

---

## 15. Summary and next steps

### What you've learned

| Topic | Form |
|---|---|
| Declare an effect | `(defeffect Name [p :T ...] :R)` |
| Perform an effect | `(perform (Name arg ...))` |
| Handle effects | `(handle expr (Name [p ...] k) body ...)` |
| Resume computation | `(resume k value)` |
| Abort computation | Omit `resume` (e.g. throw instead) |
| Multiple effects | Multiple clauses in one `handle` |
| Nested scoping | Inner `handle` shadows outer for same effect |
| Macro wrappers | `(defmacro with-x [body] (handle body ...))` |
| Interop with defer/ref/rc | Works transparently across perform/resume |
| Bridge to exceptions | Handler calls `throw!` instead of `resume` |

### stdlib effects

`stdlib/effects.tur` provides ready-made effects and handlers:

| Effect | Handler macro | What it does |
|---|---|---|
| `Write [s :cstr]` | `with-write` | Routes writes to `println` |
| `Fail [msg :cstr]` | `with-fail-throw` | Converts failures to exceptions |
| `Read []` | `with-read-console` | Reads an int from stdin |
| `GetEnv [key :cstr]` | `with-getenv` | Delegates to C `getenv(3)` |

### Further reading

- `docs/turmeric-plan.md` §10.18–10.19 — Phase 18 (shift/reset) and Phase 19 (algebraic effects) design details
- `tests/fixtures/effect-*` — full fixture test suite for every effect feature
- `docs/archive/async-await-plan.md` — planned async/await built on the effect substrate
- `docs/archive/stm-plan.md` — planned STM built on the effect substrate
