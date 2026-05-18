# Panic System vs. Exception System - Design & Implementation Plan

## Overview

This document compares panic-based error handling (Rust, Go) against exception-based error handling (Java, Python, C++, Haskell) to inform the design decision for the language's error handling mechanism.

---

## Table of Contents

1. [Definitions](#definitions)
2. [Comparison Matrix](#comparison-matrix)
3. [Design Considerations](#design-considerations)
4. [Implementation Scenarios](#implementation-scenarios)
5. [Recommendation](#recommendation)
6. [Action Items](#action-items)

---

## Definitions

### Panic System
- **Mechanism**: Unrecoverable errors that unwind the stack
- **Recovery**: Limited; typically via `catch` at thread/process boundary (Rust: `catch_unwind`, Go: `recover`)
- **Use Case**: Programmer errors, invariant violations, unrecoverable conditions
- **Examples**: Rust's `panic!`, Go's `panic`, C++ `std::terminate`

### Exception System
- **Mechanism**: Structured error values that can be caught and handled
- **Recovery**: Granular; catch at any point in the call stack
- **Use Case**: Expected error conditions, recoverable failures
- **Examples**: Java `try/catch`, Python `try/except`, C++ `try/catch`, Haskell `catch`

---

## Comparison Matrix

| Aspect | Panic System | Exception System |
|--------|--------------|------------------|
| **Recovery Granularity** | Coarse (process/thread level) | Fine (any catch site) |
| **Error Representation** | Simple message or payload | Rich type hierarchy |
| **Stack Unwinding** | Always (destructors run) | Always (destructors run) |
| **Performance** | Zero-cost when not panicking | Slight overhead for exception tables |
| **Type Safety** | Limited (payload is `Any`/`Box<dyn Any>`) | Strong (typed exceptions possible) |
| **Control Flow Clarity** | Implicit, non-local exit | Explicit catch blocks |
| **Resource Safety** | RAII ensures cleanup | RAII ensures cleanup |
| **Backpressure** | None - aborts | Can propagate or handle |
| **Composability** | Poor - hard to compose panic-handling code | Good - exceptions compose naturally |
| **Debugging** | Stack trace at panic point | Stack trace at throw and catch |
| **Library Design** | Cannot define new error types for panics | Can define custom exception types |
| **Interop** | Difficult with exception-based languages | Natural with exception-based languages |

---

## Design Considerations

### 1. Language Philosophy
- **Panic aligns with**: "Crash early, crash often" - fail-fast philosophy
- **Exception aligns with**: "Handle errors gracefully" - defensive programming

### 2. Error Taxonomy

```
Error Types:
├── Programmer Errors (Bugs)
│   ├── Type errors (caught by compiler)
│   ├── Logic errors
│   ├── Invariant violations
│   └── Resource exhaustion
│
├── Runtime Errors (Expected)
│   ├── File not found
│   ├── Network timeout
│   ├── Parse errors
│   ├── Permission denied
│   └── Invalid input
│
└── System Errors (Unrecoverable)
    ├── Out of memory
    ├── Stack overflow
    └── Hardware failure
```

**Question**: Should programmer errors and system errors use panic, while runtime errors use exceptions/Result?

### 3. Integration with Type System

#### Panic + Result (Rust Model)
```rust
// Recoverable errors: Result<T, E>
fn parse(input: &str) -> Result<Ast, ParseError> { ... }

// Unrecoverable errors: panic!
fn invariant_violated() {
    panic!("index out of bounds");
}
```

#### Exception Only (Java Model)
```java
// All errors: exceptions
public Ast parse(String input) throws ParseException { ... }

public void invariantCheck() throws AssertionError { ... }
```

#### Hybrid (C++ Model)
```cpp
// Exceptions for recoverable
Ast parse(const string& input); // throws ParseException

// assert/terminate for unrecoverable
void invariantCheck() { 
    if (!valid) std::terminate(); 
}
```

### 4. Performance Implications

| Operation | Panic | Exception (no throw) | Exception (throw) |
|-----------|-------|---------------------|-------------------|
| Normal path | 0 cost | ~0-5ns (table lookup) | N/A |
| Error path | Unwinding cost | Unwinding + dispatch | Unwinding + dispatch |
| Binary size | Minimal | Exception tables add ~5-15% |

**Note**: Modern exception implementations (e.g., LLVM's) have near-zero cost when no exception is thrown.

### 5. Interoperability

- **C FFI**: Panics cannot cross FFI boundaries safely; exceptions equally problematic
- **WASM**: Both can be represented as `unreachable` or custom error types
- **Other Languages**: Exception systems have more precedent for interop

### 6. Learning Curve
- Panic: Simpler mental model ("this should never happen")
- Exception: Requires understanding of exception hierarchies and catch semantics

---

## Implementation Scenarios

### Scenario 1: Pure Panic System

```
# All errors are panics
func parse(file: String) -> Ast {
    let contents = read_file(file) or_else panic("File not found");
    # ...
}

# Recovery only at boundaries
func main() {
    catch_panic(|| {
        parse("input.txt");
    }, |err| {
        eprintln("Fatal: {}", err);
        exit(1);
    });
}
```

**Pros**: Simple, consistent
**Cons**: Cannot recover from expected errors, poor composability

### Scenario 2: Pure Exception System

```
# All errors are exceptions
func parse(file: String) -> Ast throws ParseError, IOError {
    let contents = read_file(file)?;  # throws IOError
    # ...
}

func main() {
    try {
        let ast = parse("input.txt");
        # ...
    } catch (e: ParseError) {
        eprintln("Parse error: {}", e);
    } catch (e: IOError) {
        eprintln("IO error: {}", e);
    }
}
```

**Pros**: Fine-grained recovery, composable
**Cons**: Boilerplate, exception hierarchy complexity

### Scenario 3: Hybrid - Result + Panic

```
# Recoverable errors: Result type
func parse(file: String) -> Result<Ast, ParseError> {
    let contents = read_file(file)?;  # returns Result
    # ...
}

# Unrecoverable errors: panic
func index(vec: Vector<T>, i: Int) -> T {
    if i >= vec.len() {
        panic("Index out of bounds");
    }
    vec[i]
}

# Panic recovery at boundaries only
func main() {
    match catch_panic(|| parse("input.txt")) {
        Ok(ast) => process(ast),
        Err(e) => eprintln("Fatal: {}", e),
    }
}
```

**Pros**: Best of both worlds, explicit about recoverability
**Cons**: Two error handling mechanisms to learn, boundary between them can be blurry

### Scenario 4: Checked vs Unchecked Exceptions

```
# Checked exceptions (must handle or declare)
func parse(file: String) -> Ast throws ParseError, IOError { ... }

# Unchecked exceptions (runtime errors)
func invariant() -> Void throws AssertionError { ... }

func caller() {
    let ast = parse("file.txt");  # Must catch ParseError, IOError
    invariant();  # AssertionError is unchecked, can propagate
}
}
```

**Pros**: Compiler enforces handling of recoverable errors
**Cons**: Verbose, can lead to empty catch blocks or declaration pollution

---

## Recommendation

### Recommended Approach: Hybrid Result + Limited Panic

Based on the analysis, the following approach is recommended:

1. **Primary mechanism**: `Result<T, E>` type for all recoverable errors
2. **Panic mechanism**: Reserved for truly unrecoverable conditions (invariants, bugs)
3. **Panic recovery**: Only at process/thread boundaries, not for normal error handling
4. **Exception compatibility**: Provide interop layer for languages that expect exceptions

### Rationale

1. **Explicitness**: `Result` makes error handling explicit in function signatures
2. **Composability**: `Result` composes well with `map`, `flat_map`, `?` operator
3. **Safety**: Panic is reserved for genuine bugs, not control flow
4. **Performance**: Zero-cost for both happy path and error path
5. **Clarity**: Clear distinction between "this can fail" (Result) and "this should never happen" (panic)

### Implementation Phases

#### Phase 1: Core Result Type
- [ ] Define `Result<T, E>` enum with `Ok` and `Err` variants
- [ ] Implement `map`, `flat_map`, `map_err` combinators
- [ ] Add `?` operator for ergonomic error propagation
- [ ] Define standard error trait with `Display` and `Debug` methods

#### Phase 2: Panic Mechanism
- [ ] Define `panic!` macro with message and optional payload
- [ ] Implement stack unwinding with destructor calls
- [ ] Add `catch_unwind` for boundary recovery (limited scope)
- [ ] Ensure panic payload carries type information

#### Phase 3: Standard Library Errors
- [ ] Define error types for IO, parsing, etc.
- [ ] Create error conversion traits (`From`, `Into`)
- [ ] Establish error hierarchy (optional: trait-based)

#### Phase 4: Interop
- [ ] FFI safe panic handling (abort vs unwind)
- [ ] Exception translation layer for interop
- [ ] WASM error representation

#### Phase 5: Tooling
- [ ] Compiler warnings for unhandled Results
- [ ] Linter rules for panic usage (should be rare)
- [ ] Debugging support for panic stack traces

---

## Action Items

- [ ] **Decision**: Confirm hybrid approach (Result + limited panic) is acceptable
- [ ] **Design**: Finalize `Result` type API and combinators
- [ ] **Design**: Define panic payload type and recovery semantics
- [ ] **Research**: Survey existing implementations (Rust, Swift, Go)
- [ ] **Prototype**: Implement minimal `Result` and `panic!` in compiler
- [ ] **Evaluate**: Benchmark performance of both paths
- [ ] **Document**: Write user-facing error handling guide

---

## Open Questions

1. Should panic payload be typed or `Any`? ANSWER: I think it should be possible
2. Should there be a way to catch specific panic types? ANSWER: I think it should be possible
3. How should panic interact with generators/async? ANSWER: Look to prior art
4. Should we support `throw` as an alias for `panic` for familiarity? ANSWER: Let's converge on `panic`
5. How do we handle panics in `Drop` implementations? ANSWER: Look to prior art
6. Should we have a `Must` type that panics on `None`/`Err` automatically? ANSWER: I think so

---

## References

- Rust: `Result`, `panic!`, `catch_unwind`
- Go: `panic`, `recover`, error return values
- Java: Checked and unchecked exceptions
- Python: Exception hierarchy, `try/except/finally`
- C++: `try/catch`, `std::exception`, `noexcept`
- Haskell: `Maybe`, `Either`, `Exception` typeclass, `throw`/`catch`
- Swift: `Result`, `throws`, `try/catch`
- Zig: Error unions, `try`, `catch`

---

*Status: Draft*
*Created: 2024-01-00*
*Last Updated: 2024-01-00*
