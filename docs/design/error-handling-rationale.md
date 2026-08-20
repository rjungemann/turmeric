# Error Handling Rationale: Exceptions vs. Panic

> **Status: Superseded.** This document records the original (pre-v0.25.0)
> rationale for exception-based error handling. The `throw` / `try` / `catch`
> forms it describes were **deleted end-to-end in v0.25.0** (see CHANGELOG).
> Current Turmeric uses `Result` / `Option` for recoverable errors and
> `panic` / `catch-unwind` for unrecoverable ones -- see
> [docs/guides/error-handling-guide.md](../guides/error-handling-guide.md)
> for what is implemented today. The panic/`defer` half of this document
> still reflects current behavior; the exception half is history.

This document explains Turmeric's original choice of **exception-based error handling** and compares it with panic-based approaches.

## Executive Summary

Turmeric uses **exceptions for recoverable errors** (expected failures, I/O errors, validation failures) combined with **panics for unrecoverable errors** (programmer bugs, invariant violations, stack overflow).

**Why exceptions?**
- **Composability:** Exceptions compose naturally; functions can chain recovery logic.
- **Fine-grained recovery:** Catch at any level; propagate or handle as needed.
- **Interop:** Exception-based design aligns with Python, Java, Haskell, C++.
- **Type safety:** Typed exceptions (v2) enable compile-time checking.

**Why not pure panic?**
- **Coarse-grained recovery:** Can only catch at process/thread boundary.
- **Poor composability:** Hard to build libraries; composing panic handlers is cumbersome.
- **Limited error information:** Payload is typically `Any`/`Box<dyn Any>`.

## Comparison: Exceptions vs. Panic

### Panic System (Rust, Go)

**Mechanism:** Unrecoverable errors unwind the stack; recovery is limited to thread/process boundary.

```rust
fn main() {
    match std::panic::catch_unwind(|| risky_fn()) {
        Ok(result) => println!("{}", result),
        Err(_) => println!("panicked"),
    }
}
```

**Pros:**
- Zero-cost when not panicking (no exception table overhead).
- Clear intent: panic = programmer error.
- Simpler implementation.

**Cons:**
- Can only catch at thread boundary.
- No granular control; either catch everything or nothing.
- Hard to define custom panic payloads.
- Library design assumes either success or crash.

### Exception System (Java, Python, C++, Haskell)

**Mechanism:** Structured error values that can be caught and handled at any level.

```java
try {
    risky_fn();
} catch (IOException e) {
    System.out.println("I/O error: " + e.getMessage());
} catch (ValidationError e) {
    System.out.println("Validation failed: " + e);
}
```

**Pros:**
- Fine-grained recovery at any call level.
- Rich error types; library can define custom exceptions.
- Natural error propagation with automatic stack unwinding.
- Composable: functions can wrap and re-throw with context.

**Cons:**
- Exception handling has slight overhead (exception tables, unwinding).
- Visible control flow; exceptions can be non-local.
- Requires discipline; overly broad catch blocks mask bugs.

## Turmeric's Design

### Recovery Errors: Exceptions

Expected failures (I/O, validation, network) use **exceptions**:

```turmeric
(try
  (read-file "data.txt")
  (catch [FileNotFound]
    (handle-missing-file)))

(try
  (parse-json input)
  (catch [ParseError e]
    (println (str "Parse error: " e.message))))
```

**Why?**
- These are expected; programs should recover.
- Callers need to decide: retry, use default, report to user, etc.
- Composability: `read-file` -> `parse-json` -> `validate` can chain recovery.
- Debugging: exception traces show exactly where the error occurred.

### Programming Errors: Panic

Bugs and invariant violations use **panic**:

```turmeric
;; Assertion
(assert (> x 0) "x must be positive")

;; Explicit panic
(panic "unreachable code")

;; Index out of bounds
(vec-get vec 999)  ; panics if out of bounds
```

**Why?**
- These should never happen in a correct program.
- Catching them is usually a mistake (masking bugs).
- Stack traces are more useful than exception catching.
- Fail-fast philosophy: don't silently continue with corrupted state.

### RAII and Resource Cleanup

Both exceptions and panics trigger scope unwinding, running `defer` blocks:

```turmeric
(defn with-file [path f]
  (let [file (open-file path)]
    (defer (close-file file))
    (f file)))

;; Exception case
(try
  (with-file "data.txt" read-data)
  (catch [IOException]
    (println "I/O failed")))
;; => file is closed regardless of exception

;; Panic case
(with-file "data.txt" (fn [f] (assert false)))
;; => file is closed before panic propagates
```

## Exception Hierarchy (v1)

v1 exceptions are untyped; runtime checking:

```turmeric
(try
  (risky-operation)
  (catch [e]
    (match e
      (io-error msg) -> (handle-io msg)
      (validation-error msg) -> (handle-validation msg)
      other -> (re-throw other))))
```

## Typed Exceptions (v2)

v2 adds compile-time checking via exception types:

```turmeric
(deftype io-error (struct [message : string]))
(deftype validation-error (struct [field : string, reason : string]))

(try
  (read-file "data.txt")
  (catch [e : io-error]
    (println (str "I/O: " e.message))))
;; Compiler verifies e has type io-error
```

## Panic Vs. Assert

**`panic`** -- Programmer explicitly signals unrecoverable failure:

```turmeric
(defn divide [a b]
  (if (= b 0)
    (panic "division by zero")
    (/ a b)))
```

**`assert`** -- Programmer documents an invariant; panics if violated:

```turmeric
(defn safe-divide [a b]
  (assert (not (= b 0)) "b must not be zero")
  (/ a b))
```

Both propagate and trigger unwinding.

## Integration with Effects (v3)

In v3, exceptions interact with algebraic effects:

```turmeric
;; Exception as an effect
type _ Effect.t += Throw : exn -> never Effect.t

(try-with
  (fn []
    (read-file "data.txt"))
  (fn [e k]
    (match e
      (io-error msg) -> ...)))
```

This allows custom exception handling logic without language support; useful for serialization, retry logic, or domain-specific recovery.

## Design Decision Summary

| Aspect | Turmeric's Choice | Rationale |
|--------|-------------------|-----------|
| **Expected errors** | Exceptions | Composable, fine-grained recovery |
| **Programmer errors** | Panic | Fail-fast, clear intent |
| **Recovery API** | `try/catch` | Familiar from Python, Java, etc. |
| **Typed exceptions** (v2) | Yes | Compile-time checking for specific errors |
| **Exception tables** | Yes | Minimal overhead; acceptable trade-off |
| **Unwinding on panic** | Yes | Ensures resource cleanup via `defer` |

## See Also

- [Error Handling Guide](../guides/error-handling-guide.md) -- the current `Result` / `Option` / `panic` / `catch-unwind` model
- [Effects System Guide](../guides/effects-system-guide.md) -- effect handlers (`try-with`)
- [Threading Guide](../guides/threading-guide.md) -- error handling in threads
