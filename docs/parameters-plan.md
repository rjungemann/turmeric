# Racket-like Parameters Plan for Turmeric

> **Status:** Speculative — Future Feature  
> **Prerequisite:** Phase T19 complete (thread-local storage infrastructure), stable Turmeric compiler  
> **Target:** v2 or later  
> **Related:** [turmeric-plan.md](turmeric-plan.md), [deferred-tasks-T19-T21.md](deferred-tasks-T19-T21.md)

---

## Executive Summary

This document outlines the design and implementation of **Racket-like parameters** for Turmeric — a system for creating and modifying global, thread-local, or dynamic bindings that act as implicit parameters to functions. This enables:

1. **Implicit configuration** — Library functions can accept configuration without explicit parameter passing
2. **Dynamic scoping** — Bindings can be temporarily overridden within a scope
3. **Thread-local state** — Each thread can have independent parameter values
4. **Composition** — Multiple parameterizations can be nested and combined

The system is inspired by Racket's [parameterize](https://docs.racket-lang.org/reference/parameters.html) form but adapted to Turmeric's statically-typed, compile-to-C semantics.

---

## Design Goals

### Primary Goals
- **Type-safe**: All parameter values have known types at compile time
- **Zero-overhead**: No runtime cost when parameters are not actively modified
- **Thread-safe**: Parameters work correctly in multi-threaded contexts
- **Composable**: Parameter overrides can be nested and combined predictably

### Secondary Goals
- **Debuggable**: Clear error messages for type mismatches and usage errors
- **Interoperable**: Parameters can wrap C globals for FFI compatibility
- **Efficient**: Minimal memory overhead for parameter storage

### Non-Goals
- **Full dynamic typing**: Parameter types are static (unlike Racket's dynamic parameters)
- **Runtime parameter creation**: All parameters must be declared at compile time
- **Garbage collection**: Parameters use existing Turmeric memory management (RC/borrow)

---

## Conceptual Overview

### Core Concepts

```scheme
;; Parameter declaration - creates a new parameter with a default value
(defparameter debug-mode :bool false)

;; Parameter dereference - call the parameter like a function to get its current value
(println (debug-mode))  ; => false

;; Parameterize - temporarily binds a parameter within a scope
;; Note: binding position uses the bare name to identify the parameter;
;;       body uses call syntax to dereference it
(parameterize ([debug-mode true])
  (println (debug-mode)))  ; => true

;; Nested parameterize - inner binding overrides outer
(parameterize ([debug-mode true])
  (println (debug-mode))  ; => true
  (parameterize ([debug-mode false])
    (println (debug-mode))))  ; => false

;; After parameterize, original value is restored
(println (debug-mode))  ; => false
```

### Thread-Local Semantics

By default, parameters are **thread-local**: each thread has its own stack of bindings.

```scheme
(defparameter thread-id :int 0)

(fn main [] :void
  (parameterize ([thread-id 1])
    (thread-spawn (fn [] (println (thread-id))))))  ; => 0 (child thread inherits parent's base value)
```

### Comparison with Racket

| Feature | Racket | Turmeric |
|---|---|---|
| Dynamic typing | Yes | No (static types) |
| Runtime creation | Yes | No (compile-time only) |
| Thread-local default | No (process-global) | Yes |
| CPS transformation | No | Yes (for continuations) |
| Zero-cost when unused | No | Yes |
| Call-style dereference | Yes (`(p)`) | Yes (`(p)`) |

---

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Turmeric Compiler                               │
├─────────────────────────────────────────────────────────────────────┤
│  Elaborator (src/elab.c)                                               │
│  ├── defparameter form parsing                                        │
│  ├── parameter reference resolution                                   │
│  └── parameterize form expansion                                      │
├─────────────────────────────────────────────────────────────────────┤
│  Type System (src/types.{c,h})                                        │
│  └── ParameterType wrapper                                             │
├─────────────────────────────────────────────────────────────────────┤
│  Code Generation (src/emit.c)                                         │
│  ├── Parameter global/thread-local declaration                       │
│  ├── Parameter access helper functions                                │
│  └── parameterize scope management                                   │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Generated C Code                                  │
├─────────────────────────────────────────────────────────────────────┤
│  // Parameter storage (thread-local)                                   │
│  TUR_THREAD_LOCAL ParameterValue *param_debug_mode = NULL;           │
│                                                                         │
│  // Accessor function                                                  │
│  bool param_get_debug_mode(void) {                                     │
│    if (param_debug_mode && param_debug_mode->has_override)           │
│      return param_debug_mode->override.value_bool;                    │
│    return false;  // default                                          │
│  }                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Structures

#### Parameter Storage (C Runtime)

```c
// Parameter value storage with override stack
typedef struct ParameterBinding {
    union {
        bool value_bool;
        int64_t value_int;
        double value_float;
        void *value_ptr;
        // ... other types
    } value;
    struct ParameterBinding *prev;  // Previous binding in stack
    bool has_override;               // True if this is an override
} ParameterBinding;

// Parameter descriptor (one per parameter)
typedef struct Parameter {
    const char *name;
    ParameterType type;
    ParameterBinding *current; TUR_THREAD_LOCAL;
    union {
        bool default_bool;
        int64_t default_int;
        // ...
    } default_value;
} Parameter;
```

#### Compiler Representation

In `src/expr.h`:
```c
// Parameter definition
typedef struct ParamDef {
    Symbol *name;
    Type *type;
    Expr *default_value;
    bool is_thread_local;  // Always true in v1
} ParamDef;

// Parameter reference
typedef struct ParamRef {
    Symbol *name;
    Span span;
} ParamRef;

// Parameterize form
typedef struct Parameterize {
    ParamBinding *bindings;  // Array of (param_name, value) pairs
    Expr *body;
    Span span;
} Parameterize;
```

---

## Implementation Phases

Parameters split into two tracks based on whether they require runtime type information or first-class parameter values (which depend on HKTs):

- **Static parameters** (in scope, v1): All parameters declared at compile time with known types. This is the primary v1 feature.
- **Runtime parameters** (deferred, post-HKT): Parameters created at runtime (`make-parameter`), parameter predicates, and higher-order parameter operations. These require first-class parameter values and are deferred until HKT support lands.

---

### Static Parameters (In Scope — v1)

#### Phase P0: Core Parameter System (Prerequisite)

**Goal:** Establish the basic parameter declaration and reference infrastructure.

**Exit Criterion:** Can declare parameters, reference them, and compile programs that use them.

##### Type System (`src/types.{c,h}`)
- [ ] Add `TYPE_PARAMETER` kind to `TypeKind` enum
- [ ] Add `ParameterType` struct to represent parameter types
- [ ] Add parameter type equality checking

##### Elaborator (`src/elab.c`)
- [ ] Parse `defparameter` form:
  ```scheme
  (defparameter name :type default-value)
  ```
- [ ] Validate parameter name is a symbol
- [ ] Validate type annotation is present and valid
- [ ] Elaborate default value expression
- [ ] Register parameter in global scope
- [ ] Parse parameter dereference: recognize `(param-name)` (zero-arg call on a parameter symbol) as a dereference expression
- [ ] Emit a compile-time error (`TUR-E0105`) when a parameter symbol appears in expression position without call syntax
- [ ] Emit a compile-time error (`TUR-E0106`) when a parameter symbol is called with one or more arguments
- [ ] Add `elab_resolve_param` helper to resolve parameter symbols

##### Code Generation (`src/emit.c`)
- [ ] Emit parameter storage as `TUR_THREAD_LOCAL` variables
- [ ] Generate getter function for each parameter
- [ ] Lower `(*param*)` dereference expressions to calls to the generated getter function

##### Stdlib (`stdlib/parameters.tur`)
- [ ] Create `stdlib/parameters.tur` with core parameter types and helpers

##### Fixtures
- [ ] `tests/fixtures/parameters/basic.tur` — Basic declaration and reference
- [ ] `tests/fixtures/parameters/types.tur` — Various type tests
- [ ] `tests/fixtures/errors/param-undefined.tur` — Reference to undefined parameter

#### Phase P1: Parameterize Form (Core Feature)

**Goal:** Implement the `parameterize` form for temporary binding overrides.

**Exit Criterion:** Can temporarily override parameter values within a scope.

##### Elaborator (`src/elab.c`)
- [ ] Parse `parameterize` form:
  ```scheme
  (parameterize ([param1 value1] [param2 value2] ...) body...)
  ```
- [ ] Validate all parameter names exist and are parameters
- [ ] Validate value types match parameter types
- [ ] Transform `parameterize` into a CPS-style scope with setup/teardown

##### Code Generation (`src/emit.c`)
- [ ] Generate push/pop logic for parameter override stack
- [ ] Ensure proper cleanup on early exit (use `defer` or similar)
- [ ] Handle nested `parameterize` correctly (stack discipline)

##### Fixtures
- [ ] `tests/fixtures/parameters/parameterize.tur` — Basic parameterize usage
- [ ] `tests/fixtures/parameters/nested.tur` — Nested parameterize scopes
- [ ] `tests/fixtures/errors/param-type-mismatch.tur` — Type mismatch in parameterize

#### Phase P2: Thread-Local Semantics

**Goal:** Ensure parameters work correctly in multi-threaded contexts.

**Exit Criterion:** Parameters behave correctly when accessed from multiple threads.

##### Runtime (`src/emit.c` codegen)
- [ ] Ensure each parameter's storage is `TUR_THREAD_LOCAL`
- [ ] Each thread gets independent override stack
- [ ] Child threads inherit parent's base parameter values

##### Borrow Checker (`src/borrow_check.c`)
- [ ] Validate that parameter values captured in closures satisfy `Send` if used across threads
- [ ] Add diagnostics for invalid parameter captures

##### Fixtures
- [ ] `tests/fixtures/parameters/thread-local.tur` — Thread-local behavior
- [ ] `tests/fixtures/parameters/thread-inherit.tur` — Child thread inheritance
- [ ] `tests/fixtures/parameters/thread-override.tur` — Thread-specific overrides

#### Phase P3: Optimizations and Stdlib Ergonomics

**Goal:** Optimize common access patterns and add macro-level ergonomics.

**Exit Criterion:** Parameter access is optimized; common patterns have stdlib helpers.

##### Optimizations
- [ ] **Inline access**: When a parameter has no overrides in scope, access it directly
- [ ] **Const propagation**: If parameter value is known at compile time, use it directly
- [ ] **Dead code elimination**: Remove unused parameter declarations

##### Stdlib Extensions (`stdlib/parameters.tur`)
- [ ] `parameterize*` — parameterize with a single parameter-value pair (variadic)
- [ ] `with-parameter` — macro for single-parameter parameterize
- [ ] `param+`, `param*`, etc. — combinators for numeric parameters

##### Fixtures
- [ ] `tests/fixtures/parameters/combinators.tur` — Parameter combinator tests
- [ ] `tests/fixtures/parameters/performance.tur` — Performance-sensitive usage

---

### Runtime Parameters (Deferred — Post-HKT)

> **Prerequisite:** Higher-kinded types (HKT) support in the type system, allowing `Parameter<T>` to be a first-class value. See [turmeric-plan.new.md](turmeric-plan.new.md).
>
> These features require parameters to be first-class runtime values. Without HKTs, `make-parameter` has no expressible type, `parameter?` has no sensible static semantics, and parameter-of-parameter nesting cannot be typed.

#### Phase R0: First-Class Parameter Values

**Goal:** Allow parameters to be created and passed as values at runtime.

**Exit Criterion:** `make-parameter` works; parameters can be stored in variables and passed to functions.

##### Type System (`src/types.{c,h}`)
- [ ] Add `Parameter<T>` as a first-class HKT in the type system
- [ ] Define subtyping/coercion rules between `Parameter<T>` and `T`

##### Elaborator (`src/elab.c`)
- [ ] Support `make-parameter` for creating parameters from values:
  ```scheme
  (def my-param (make-parameter :int 42))
  ```
- [ ] Support `parameter?` predicate
- [ ] Support `parameter-procedure` to convert parameter to a thunk

##### Fixtures
- [ ] `tests/fixtures/parameters/make-parameter.tur` — Runtime parameter creation
- [ ] `tests/fixtures/parameters/parameter-predicate.tur` — `parameter?` usage

#### Phase R1: Parameter of Parameters

**Goal:** Allow parameters whose values are themselves parameters.

**Exit Criterion:** A `Parameter<Parameter<T>>` can be declared, bound, and dereferenced.

> See open question 2 (answered: defer to HKT work).

##### Notes
- Requires `Parameter<T>` to be `Copy` or have a well-defined ownership story
- Accessor codegen must emit a two-level dereference
- Semantics of `parameterize` over a parameter-valued parameter must be defined (rebind the outer, or the inner, or both?)

##### Fixtures
- [ ] `tests/fixtures/parameters/nested-param.tur` — Parameter-of-parameter usage

---

## Syntax and Semantics

### Declaration

```scheme
;; Basic declaration with type and default
(defparameter debug :bool false)

;; Declaration with complex default
(defparameter max-iterations :int (* 1000 1000))

;; Declaration referencing other bindings
(defn get-default-timeout [] :int 5000)
(defparameter timeout :int (get-default-timeout))
```

### Dereference

Parameters must be **called with no arguments** to get their current value, just as in Racket. The bare name identifies the parameter (used in `defparameter` and `parameterize` binding positions); the call form dereferences it.

```scheme
;; Dereference in a condition
(defn log [msg :cstr] :void
  (when (debug)
    (println msg)))

;; Dereference in an expression
(defn scale [x :int] :int
  (* x (scale-factor)))

;; Error: bare parameter name in expression position
(println debug)  ; TUR-E0105: debug is a parameter; use (debug) to dereference
```

### Parameterize

```scheme
;; Single binding
(parameterize ([debug true])
  (do-something))

;; Multiple bindings
(parameterize ([debug true]
               [timeout 1000])
  (do-something))

;; Nested — bare name in binding position, call syntax in body
(parameterize ([a 1])
  (println (a))  ; => 1
  (parameterize ([a 2])
    (println (a))))  ; => 2
  ; a restored to 1 here
  (println (a)))  ; => 1
```

### Error Cases

```scheme
;; Error: undefined parameter
(undefined-param)
; TUR-E0100: undefined parameter: undefined-param

;; Error: bare parameter name in expression position (must call to dereference)
(println count)
; TUR-E0105: count is a parameter; use (count) to dereference

;; Error: type mismatch in parameterize
(defparameter count :int 0)
(parameterize ([count true]) ...)
; TUR-E0102: parameterize: expected :int, got :bool for parameter count

;; Error: not a parameter
(defn foo [] :int 42)
(parameterize ([foo 10]) ...)
; TUR-E0101: parameterize: foo is not a parameter

;; Error: calling a parameter with arguments
(count 1)
; TUR-E0106: parameter count takes no arguments; use (count) to dereference
```

---

## Compilation Strategy

### Parameter Access

Parameter dereferences — `(param)` call forms — are compiled to getter function calls. The bare name `param` is only valid in `defparameter` and `parameterize` binding positions; using it in expression position is a compile-time error.

```scheme
;; Turmeric
(defparameter x :int 0)
(println (x))
```

```c
// Generated C
TUR_THREAD_LOCAL ParameterBinding *param_x = NULL;

int64_t param_get_x(void) {
    if (param_x && param_x->has_override) {
        return param_x->value.int64;
    }
    return 0;  // default
}

int main(void) {
    println_int64(param_get_x());
    return 0;
}
```

### Parameterize Compilation

`parameterize` is compiled using a stack-based approach:

```scheme
;; Turmeric
(parameterize ([x 10])
  (println (x)))
```

```c
// Generated C
void parameterize_1(void) {
    // Save current binding
    ParameterBinding old_x = {0};
    if (param_x) {
        old_x = *param_x;
    }
    
    // Push new binding
    ParameterBinding new_x = {
        .value.int64 = 10,
        .prev = param_x,
        .has_override = true
    };
    param_x = &new_x;
    
    // Body
    println_int64(param_get_x());
    
    // Pop binding (via defer or explicit cleanup)
    param_x = old_x.prev ? old_x.prev : NULL;
}
```

### Optimization: Direct Access

When the compiler can prove no overrides are active:

```scheme
;; When no parameterize is in scope
(defparameter x :int 0)
(defn get-x [] :int (x))
```

```c
// Optimized to direct access
int64_t get_x(void) {
    return 0;  // Inlined default
}
```

### Optimization: Thread-Local Direct Access

For frequently-accessed parameters with no current overrides:

```c
// When param_x has no override in current scope
int64_t param_get_x(void) {
    // Direct thread-local access if no override stack
    if (!param_x || !param_x->has_override) {
        return 0;  // default is const
    }
    return param_x->value.int64;
}
```

---

## Type System Integration

### Parameter Types

Parameters support all Turmeric types that are `Copy` (can be copied by value):

| Type | Supported | Notes |
|---|---|---|
| `int`, `uint`, `float`, `double` | Yes | Copied by value |
| `bool` | Yes | Copied by value |
| `cstr` | Yes | Pointer, but immutable |
| `ptr<T>`, `ref<T>`, `rc<T>` | No | Cannot be copied safely |
| Structs | Yes | If all fields are `Copy` |
| Tuples | Yes | If all elements are `Copy` |
| `cont<T>` | No | Continuations are not `Copy` |

### Type Checking

- [ ] Parameter declaration type must be `Copy`
- [ ] Parameter default value must match declared type
- [ ] `parameterize` value must match parameter type
- [ ] Parameter values captured in closures must satisfy `Send` if closure crosses threads

---

## Memory Management

### Ownership Semantics

- Parameter storage is global/thread-local, not owned by any specific value
- Parameter values are copied by value (for `Copy` types)
- No reference counting needed for parameter values themselves
- Override stack nodes are allocated on the stack (when possible) or in a thread-local arena

### Lifetimes

- Parameter override stack lifetime matches the `parameterize` scope
- Override nodes are automatically popped when scope exits
- No manual memory management required from user code

---

## Interaction with Other Features

### Algebraic Effects

Parameters and effects compose naturally:

```scheme
(defeffect Log [msg :cstr] :void)

(defparameter log-level :int 1)

(defn do-log [msg :cstr] :void
  (when (>= (log-level) 2)
    (perform (Log msg))))

(parameterize ([log-level 3])
  (handle (do-log "test")
    (Log [msg] k) (println msg)))
```

### Macros

Macros can expand to parameter uses:

```scheme
(defmacro with-debug [[msg :cstr] body ...]
  `(parameterize ([debug true])
     ,@body))

(with-debug () (println "debugging"))
```

### Typeclasses

Parameters can be used in typeclass methods:

```scheme
(defclass Show a)
  (defn show [x :a] :cstr))

(defparameter show-depth :int 0)

(definstance Show (List a) where (Show a)
  (defn show [xs :(List a)] :cstr
    (when (> (show-depth) 0)
      (list->string xs))))
```

### FFI

Parameters can wrap C globals for FFI compatibility:

```scheme
;; C declaration: extern int some_global;
(extern-c some_global :int)

;; Wrap in parameter
(defparameter some-global :int (unsafe-get-global some_global))

;; Now can use parameterize
(parameterize ([some-global 42])
  (call-c-function))
```

---

## Error Codes

Add to `src/diag.c`:

| Code | Message | Severity |
|---|---|---|
| `TUR_E0100_PARAM_UNDEFINED` | `undefined parameter: %s` | Error |
| `TUR_E0101_PARAM_NOT_PARAMETER` | `%s is not a parameter` | Error |
| `TUR_E0102_PARAM_TYPE_MISMATCH` | `parameterize: expected %s, got %s for parameter %s` | Error |
| `TUR_E0103_PARAM_NOT_COPY` | `parameter type must be Copy: %s` | Error |
| `TUR_E0104_PARAM_DEFAULT_TYPE` | `parameter default value type mismatch: expected %s, got %s` | Error |
| `TUR_E0105_PARAM_BARE_DEREF` | `%s is a parameter; use (%s) to dereference` | Error |
| `TUR_E0106_PARAM_WRONG_ARITY` | `parameter %s takes no arguments; use (%s) to dereference` | Error |

---

## Testing Strategy

### Unit Fixtures

- [ ] Basic declaration and access
- [ ] Default values of various types
- [ ] Simple parameterize
- [ ] Nested parameterize
- [ ] Parameterize with multiple bindings
- [ ] Parameter value persistence after scope

### Integration Fixtures

- [ ] Parameters in functions
- [ ] Parameters in loops
- [ ] Parameters with conditionals
- [ ] Parameters in macros
- [ ] Parameters with typeclasses
- [ ] Parameters with effects

### Thread Fixtures

- [ ] Thread-local isolation
- [ ] Child thread inheritance
- [ ] Thread-specific overrides
- [ ] Parameters with `Send`/`Sync` types

### Error Fixtures

- [ ] Undefined parameter reference
- [ ] Non-parameter in parameterize
- [ ] Type mismatch in parameterize
- [ ] Non-Copy parameter type
- [ ] Default value type mismatch

### Performance Fixtures

- [ ] Deeply nested parameterize (stack usage)
- [ ] Many parameter references (inlining)
- [ ] Frequent parameter access (optimization)

---

## Open Questions

1. **Naming convention**: Should parameters have a naming convention (e.g., `*` prefix/suffix)?
   - **Pro**: Clear visual distinction, harder to accidentally shadow
   - **Con**: Extra typing, not idiomatic Lisp
ANSWER: No naming convention. Use bare names like Racket.

2. **Parameter of parameters**: Should we allow parameters whose values are themselves parameters?
   - **Pro**: More flexible
   - **Con**: Complex semantics, type system complications
ANSWER: Disallow for v1, add a task to revisit as part of HKT work (add to turmeric-plan.new.md)

3. **Dynamic parameter creation**: Should we support runtime parameter creation (like Racket)?
   - **Pro**: More expressive
   - **Con**: Requires runtime type information, conflicts with static typing
ANSWER: Disallow for v1, add a task to revisit as part of HKT work (add to turmeric-plan.new.md)

4. **Parameter groups**: Should we support grouping parameters for batch updates?
   - **Pro**: Useful for related configuration
   - **Con**: Adds complexity
ANSWER: Out of scope for v1

5. **Serialization**: Should parameter values be serializable for distributed computing?
   - **Pro**: Enables interesting use cases
   - **Con**: Out of scope for v1
ANSWER: Out of scope for v1

---

## Related Work

### Racket Parameters

Racket's parameter system is the primary inspiration:
- https://docs.racket-lang.org/reference/parameters.html
- `make-parameter`, `parameterize`, `parameter-procedure`
- Process-global by default, but can be made thread-local

### Other Languages

| Language | Feature | Notes |
|---|---|---|
| Python | `contextvars` | Thread-local context variables |
| Java | `ThreadLocal<T>` | Thread-local storage |
| Rust | `thread_local!` | Thread-local statics |
| Haskell | `ReaderT` | Reader monad for implicit parameters |
| Clojure | `binding` | Dynamic vars with thread-local semantics |

### Key Differences from Racket

1. **Static typing**: Turmeric parameters have fixed, known types
2. **Thread-local by default**: Racket parameters are process-global by default
3. **Compile-time declaration**: All parameters must be declared at compile time
4. **No guard procedures**: Racket allows guards for parameter values; Turmeric uses type system
5. **Same call-style dereference**: Both Racket and Turmeric require calling the parameter with no arguments to read its value — `(p)` in both cases. Bare use of the name in expression position is an error in Turmeric (in Racket it's valid but refers to the procedure object).

---

## Appendix A: Example Use Cases

### Configuration

```scheme
;; Library code
(defparameter log-level :int 1)

(defn log [msg :cstr] :void
  (when (>= (log-level) 2)
    (println msg)))

;; User code
(defn process-data [data :cstr] :void
  (parameterize ([log-level 3])
    (log "Processing...")
    ;; ... processing ...
    (log "Done")))
```

### Testing

```scheme
(defparameter test-mode :bool false)
(defparameter test-verbose :bool false)

(defn assert [condition :bool] :void
  (when (not condition)
    (when (test-verbose)
      (print-stack-trace))
    (when (test-mode)
      (panic "assertion failed"))))

;; Test code
(parameterize ([test-mode true]
               [test-verbose true])
  (run-tests))
```

### Performance Profiling

```scheme
(defparameter profile-enabled :bool false)
(defparameter profile-output :cstr "profile.log")

(defn profile [name :cstr thunk :(fn [] :a)] :a
  (if (profile-enabled)
    (let [start (get-time)]
      (let [result (thunk)]
        (log-profile name (- (get-time) start))
        result))
    (thunk)))

;; Enable profiling for specific code
(parameterize ([profile-enabled true])
  (my-expensive-computation))
```

### Localization

```scheme
(defparameter locale :cstr "en_US")

(defn translate [key :cstr] :cstr
  (locale-get-string (locale) key))

;; Temporarily switch locale
(parameterize ([locale "fr_FR"])
  (println (translate "hello")))
```

---

## Appendix B: Alternative Designs Considered

### Design 1: Implicit Parameters via Typeclasses

Instead of a dedicated parameter system, use typeclasses:

```scheme
(defclass Parameter a)
  (defn get :a))

(definstance Parameter Int
  (defn get [] :int 0))
```

**Rejected because:** Less ergonomic, harder to optimize, doesn't support dynamic scoping naturally.

### Design 2: Global Variables with Macros

Use global variables with macros for scoping:

```scheme
(defglobal debug :bool false)

(defmacro with-debug [body ...]
  `(let [old-debug debug]
     (set! debug true)
     ,@body
     (set! debug old-debug)))
```

**Rejected because:** Manual management, easy to forget to restore, doesn't compose well, not thread-safe by default.

### Design 3: Continuation-Passing Style

Compile parameterize to CPS:

```scheme
(parameterize ([x 10]) body)
;=> (let-cont k () body (with-parameter x 10 k))
```

**Rejected because:** More complex compilation, harder to optimize, and Turmeric already has effects for CPS.

### Design 4: Stack-Based (Chosen Design)

Use a thread-local stack of bindings. This is the chosen design because:
- Matches Racket's semantics
- Easy to understand and implement
- Compatible with existing thread-local infrastructure
- Easy to optimize

---

## Appendix C: Performance Considerations

### Access Cost

| Scenario | Cost | Notes |
|---|---|---|
| No override active | 1 function call + branch | Can be inlined |
| Override active | 1 function call + branch + dereference | Stack traversal |
| Deep override stack | O(n) where n = depth | Rare in practice |

### Memory Cost

| Per Parameter | Cost |
|---|---|
| Storage | 1 thread-local pointer |
| Override node | ~16 bytes (value + prev pointer + flags) |

### Optimization Opportunities

1. **Inline getters**: For parameters with no overrides, inline to direct value
2. **Const propagation**: Replace with constant when value is known
3. **Dead code elimination**: Remove unused parameters
4. **Stack allocation**: Allocate override nodes on stack when possible
5. **Flatten stack**: Use array-based stack for better cache locality

---

## Appendix D: Implementation Checklist

### Static Parameters (v1)

- [ ] Phase P0: Core parameter system
- [ ] Phase P1: Parameterize form
- [ ] Phase P2: Thread-local semantics
- [ ] Phase P3: Optimizations and stdlib ergonomics
- [ ] Documentation in user manual
- [ ] Migration guide from global variables (if applicable)
- [ ] Performance benchmarking

### Runtime Parameters (Post-HKT)

- [ ] Phase R0: First-class parameter values (`make-parameter`, `parameter?`, `parameter-procedure`)
- [ ] Phase R1: Parameter of parameters
