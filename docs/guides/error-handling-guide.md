# Error Handling Guide

This guide covers the Turmeric error handling story: `Result<T, E>`, `panic`, `must!`, and guidance on when to use each.

## Overview

Turmeric offers two error handling strategies:

1. **`Result<T, E>`** — recoverable errors; use when callers should handle failure.
2. **`panic`** — unrecoverable errors; use for programming mistakes and invariant violations.

## `Result<T, E>`

A `result` value is either `(ok value)` or `(err error)`. The underlying representation is a heap-allocated struct `{ bool is_ok; int64_t ok_val; int64_t err_val; }` returned as `ptr<void>`.

### Constructors

```turmeric
(ok 42)        ;; Result<int, _>: success
(err 99)       ;; Result<_, int>: failure
```

### Predicates

```turmeric
(ok? r)        ;; => bool
(err? r)       ;; => bool
```

### Extractors

```turmeric
(ok-val r)     ;; => int (unsafe; check ok? first)
(err-val r)    ;; => int (unsafe; check err? first)
```

### Safe unwrapping

```turmeric
(result-unwrap r)            ;; unwrap or print error to stderr and return 0
(result-unwrap-or r default) ;; unwrap or return default
(result-expect r "message")  ;; unwrap or panic with message
(result-must r)              ;; unwrap or panic with default message
(result-must-msg r "msg")    ;; unwrap or panic with custom message
```

### Combinators

```turmeric
(result-map r f)             ;; apply f to ok value
(result-map-err r f)         ;; apply f to err value
(result-flat-map r f)        ;; f returns a result; chain
(result-or r alternative)    ;; return r if ok, else alternative
(result-or-else r f)         ;; f produces alternative on err
```

### Collection utilities

```turmeric
(result-collect vec)         ;; (vec result) -> result<vec, E>; first err wins
(result-partition vec)       ;; -> pair; use result-partition-ok / result-partition-err
(result-partition-ok pair)   ;; -> vec of ok values
(result-partition-err pair)  ;; -> vec of err values
```

### Context helpers

```turmeric
(ok-or opt err-val)          ;; option -> result
(err-context r context-str)  ;; wrap error with additional context string
```

---

## `panic`

`panic` terminates the program unconditionally. Use it for:

- Bugs and invariant violations that cannot be recovered from.
- Assertions in test code.
- Unreachable code branches.

```turmeric
(panic "something went wrong")
```

This prints `panic: something went wrong` to stderr and calls `abort()`.

### Double-panic guard

If `tur_panic` is called during a panic (e.g. in a destructor/defer chain), it prints `double panic: aborting` and calls `abort()` immediately.

---

## `must!` and `must-msg!`

These macros unwrap an `option` or `result`, panicking on failure:

```turmeric
(must! (some 42))                          ;; => 42
(must! (none))                             ;; panic: option-must: called on none

(must-msg! (some 42) "expected a value")   ;; => 42
(must-msg! (none) "expected a value")      ;; panic: expected a value
```

For `result`, use the function forms directly:

```turmeric
(result-must (ok 7))                       ;; => 7
(result-must (err 99))                     ;; panic: result-must: called on err
(result-must-msg (ok 7) "failed")          ;; => 7
(result-must-msg (err 99) "failed")        ;; panic: failed
```

---

## `option-must` and `option-expect`

```turmeric
(option-must (some 42))                    ;; => 42
(option-must (none))                       ;; panic: option-must: called on none
(option-expect (some 42) "want value")     ;; => 42
(option-expect (none) "want value")        ;; panic: want value
```

---

## `ignore!`

Explicitly discard a result to suppress unused-result warnings (when the linter is enabled):

```turmeric
(ignore! (some-fn-returning-result))
```

---

## Standard library error types

All error types are in `stdlib/exn.tur`:

| Type | Fields | Constructor |
|---|---|---|
| `Error` | `message: cstr`, `cause: ptr<void>` | `(Error. msg cause)` |
| `IoError` | `message: cstr`, `errno_: int` | `(IoError. msg errno)` |
| `ParseError` | `message: cstr`, `line`, `col`, `file: cstr` | `(ParseError. msg line col file)` |
| `ValidationError` | `message: cstr`, `field: cstr` | `(ValidationError. msg field)` |
| `NotFoundError` | `what: cstr` | `(NotFoundError. what)` |
| `PermissionError` | `message: cstr`, `path: cstr` | `(PermissionError. msg path)` |

---

## When to use what

| Situation | Approach |
|---|---|
| Caller might handle the failure (e.g. file not found) | `result` |
| Programming error / violated invariant | `panic` |
| Unwrapping a result you are confident is ok | `result-must` / `must!` |
| Optional value that must be present | `option-must` / `must!` |
| Discarding a result intentionally | `ignore!` |

---

## Deferred

The following features are planned but not yet implemented:

- `?` operator — Phase R1; short-circuit error propagation.
- `catch-unwind` — Phase R2; catch panics at a boundary.
- `--warn-unused-result` compiler flag — Phase R6.
- `--lint-panic` compiler flag — Phase R6.

## Panic + Continuation/Effects Boundary Semantics (Phase R6)

When a panic occurs inside an effect handler or continuation:

1. **Effect handlers**: Panics that occur during effect handling are caught by the
   `catch-unwind` boundary of the `with-handler` form. If no `catch-unwind` is present,
   the panic propagates normally through the effect handler chain.

2. **Continuations**: Panics that occur in code that has captured a continuation via
   `shift`/`reset` will unwind the stack up to the continuation boundary. If the
   continuation is resumed after a panic, the panic state is cleared and normal execution
   continues. This means `(shift k ...)` forms do NOT automatically re-panic on resume.

3. **Defer**: Defer thunks are fired during panic unwinding (Phase 17 mechanism).
   If a defer thunk itself panics, the double-panic guard triggers `abort()`.

## Panic + Async Task Semantics (Phase R6)

When panics interact with async tasks:

1. **Panic in async task**: A panic that occurs inside an async task is caught at the
   task boundary. The task's future will resolve to an `Err` value containing the panic
   payload. This can be caught using `catch-unwind` at the join point.

2. **Panic during task cancellation**: If a task is cancelled while panicking, the
   panic takes precedence. The cancellation is a no-op and the panic propagates.

3. **Panic in async main**: In an async main function, an uncaught panic will terminate
   the program with a nonzero exit code after all defer thunks have fired.

4. **WASM target**: When targeting WebAssembly, panics are lowered to the WebAssembly
   `unreachable` instruction, which traps the WebAssembly execution. This is consistent
   with Rust's panic behavior in WASM.
