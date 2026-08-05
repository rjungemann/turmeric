---
title: Contract Types Guide
category: Error Handling
description: Contract types: `#refine{ x : T | p }`, `:pre`/`:post` annotations, FFI contracts
---

# Contract Types Guide

> For the runtime contract macros (`assert!`, `require!`, `ensure!`,
> `invariant!`), see [Error Handling Guide](error-handling-guide.md).

Contract types add **runtime-checked predicates** to types as a first-class language feature.
A contract type `#refine{ x : T | p }` represents values of type `T` that satisfy predicate `p`,
verified automatically at function boundaries and assignment sites.

---

## Why `#refine{...}`?

Contract types used to be spelled with bare curly braces, `{ x : T | p }`.
Bare `{...}` is now SRFI-105 curly-infix in every dialect (so `{a + b}`
reads as `(+ a b)` in plain s-expression Turmeric, no `#lang sweet-exp`
required), and contract types moved to the `#refine{...}` data-literal form
alongside the rest of the `#`-prefixed family (`#map{...}`, `#set{...}`,
`#row{...}`, `#r{...}`). The semantics are identical to the old form -- only
the reader entry point changed.

---

## Overview

The existing `assert!`, `require!`, and `ensure!` macros in `stdlib/contract.tur` are
imperative guards -- you place them manually inside function bodies. Contract types are
**declarative annotations**: you write the predicate in the type, and the compiler inserts
the checks at every crossing point.

| Approach | Where you write it | When it runs |
|---|---|---|
| `require!` macro (today) | Inside the function body | When execution reaches that line |
| Contract type `:pre` (CT1) | On the `defn` declaration | Automatically at every call site |
| Inline contract `#refine{ x : T \| p }` (CT1) | In the parameter or return type | At every use of that type |

Contract predicates are **ordinary Turmeric expressions** -- no special syntax beyond the
`#refine{ x : T | p }` wrapper. They are **pure by default**; the elaborator enforces `#fx{}` on
all predicate expressions.

---

## Syntax

### Inline Contract Type

```turmeric no-check
;; A non-negative integer
(deftype Nat #refine{ x : int | (>= x 0) })

;; Inline in a function signature
(defn sqrt [x : #refine{ y : double | (>= y 0) }] : double
  ...)
```

```sweet-exp
;; A non-negative integer
deftype Nat #refine{ x : int | (>= x 0) }

;; Inline in a function signature
defn sqrt [x : #refine{ y : double | (>= y 0) }] : double
  ...
```

The bound variable (`x`, `y`) is local to the predicate expression. It refers to the
value being checked, not to any name in the surrounding scope.

### Pre- and Post-Conditions

```turmeric
(defn divide [x : int, y : int] : int
  :pre  (not= y 0)
  :post (= (* result y) x)
  (/ x y))
```

```sweet-exp
defn divide [x : int, y : int] : int :pre (not= y 0) :post (= (* result y) x)
  (/ x y)
```

- **`:pre`** -- checked at function entry, before any user code runs.
- **`:post`** -- checked before each return; the return value is bound to `result` inside
  the predicate expression.

### Contract on a `defn`

```turmeric
(defn safe-sqrt [x : double] : double
  :pre  (>= x 0.0)
  :post (>= result 0.0)
  (sqrt x))
```

```sweet-exp
defn safe-sqrt [x : double] : double :pre (>= x 0.0) :post (>= result 0.0)
  sqrt(x)
```

### Contract on a Struct Field

```turmeric no-check
(defstruct BoundedBuffer [
  data  : (vec int)
  index : #refine{ i : int | (and (>= i 0) (< i (vec/len data))) }])
```

```sweet-exp
defstruct BoundedBuffer [
  data  : (vec int)
  index : #refine{ i : int | (and (>= i 0) (< i (vec/len data))) }]
```

The predicate for a struct field may reference other fields in scope at elaboration time.

### FFI Contract

```turmeric
(extern-c sqlite3_column_int [stmt : ptr, col : int] : int
  :pre  (and (not= stmt null) (>= col 0))
  :post (>= result 0))
```

```sweet-exp
extern-c sqlite3_column_int [stmt : ptr, col : int] : int :pre (and (not= stmt null) (>= col 0)) :post (>= result 0)
```

---

## Examples

### Bounds-Checked Vector Access

The contract documents and enforces the requirement at the call site. Inside the body,
no redundant check is needed:

```turmeric no-check
(defn vec-get-checked [v : (vec a), i : #refine{ x : int | (and (>= x 0) (< x (vec/len v))) }] : a
  (vec/get-unsafe v i))

;; At the call site, the elaborator inserts:
;;   assert((3 >= 0) && (3 < vec_len(v)), "contract violated: i in bounds")
;;   vec_get_unsafe(v, 3)
(defn example [v : (vec int)] : int
  (vec-get-checked v 3))
```

```sweet-exp
defn vec-get-checked [v : (vec a), i : #refine{ x : int | (and (>= x 0) (< x (vec/len v))) }] : a
  vec/get-unsafe(v i)

;; At the call site, the elaborator inserts:
;;   assert((3 >= 0) && (3 < vec_len(v)), "contract violated: i in bounds")
;;   vec_get_unsafe(v, 3)
defn example [v : (vec int)] : int
  vec-get-checked(v 3)
```

### Gradual Typing with Contracts

Accept an untyped value and narrow it at runtime:

```turmeric no-check
(defn ensure-positive [x : any] : #refine{ y : int | (>= y 0) }
  :contract (>= x 0)
  x)

(defn pipeline [] : #refine{ y : int | (>= y 0) }
  (ensure-positive 42))
```

```sweet-exp
defn ensure-positive [x : any] : #refine{ y : int | {y >= 0} }
  :contract {x >= 0}
  x

defn pipeline [] : #refine{ y : int | {y >= 0} }
  ensure-positive(42)
```

### API Boundary Validation via FFI

Contracts are checked before the C call and on return:

```turmeric
(extern-c sqlite3_column_int [stmt : ptr, col : int] : int
  :pre  (and (not= stmt null) (>= col 0))
  :post (>= result 0))
```

```sweet-exp
extern-c sqlite3_column_int [stmt : ptr, col : int] : int :pre (and (not= stmt null) (>= col 0)) :post (>= result 0)
```

### Pre- and Post-Conditions

```turmeric
(defn safe-sqrt [x : double] : double
  :pre  (>= x 0.0)
  :post (>= result 0.0)
  (sqrt x))
```

```sweet-exp
defn safe-sqrt [x : double] : double :pre (>= x 0.0) :post (>= result 0.0)
  sqrt(x)
```

---

## Where Checks Are Inserted

| Site | When checked |
|---|---|
| Function entry | `:pre` predicates; `#refine{ x : T \| p }` on parameters |
| Function return | `:post` predicates; `#refine{ x : T \| p }` on return type |
| Let binding | `#refine{ x : T \| p }` on the bound variable's type |
| `extern-c` call | `:pre` before call; `:post` on return value |

---

## Contract Failure

By default, a violated contract calls `panic`. A custom handler can be registered:

```turmeric
(set-contract-handler! (fn [msg location] : unit
  (log/error (str "Contract violated at " location ": " msg))))
```

```sweet-exp
set-contract-handler!
  fn [msg location] : unit
    log/error $ str "Contract violated at " location ": " msg
```

For scoped overrides:

```turmeric
(with-contract-handler my-handler
  (do-something-with-contracts))
```

```sweet-exp
with-contract-handler my-handler
  do-something-with-contracts()
```

---

## Build Modes

Contract types are enabled by default.

| Build | Contracts |
|---|---|
| `just build` (debug) | Always active |
| `just release` | Stripped by default |
| `just release` with `--keep-contracts` | Retained (safety-critical code) |

Contracts are compile-time insertions. A release build with no flag produces the same
binary as code written without contract annotations.

---

## Error Codes

| Code | Message |
|---|---|
| `TUR_E0400` | Contract violated: `{predicate}` is false for value `{value}` |
| `TUR_E0401` | Postcondition violated: `{predicate}` is false for result `{value}` |

Run `tur explain TUR_E0400` for full diagnostic output with source location and predicate text.

---

## Interaction with Existing Features

| Feature | Notes |
|---|---|
| `stdlib/contract.tur` (`assert!`, `require!`, `ensure!`) | Unchanged -- macros are imperative guards; contract types are declarative annotations |
| Borrow checker | Orthogonal -- contracts are runtime; borrow checking is compile-time |
| Typeclasses | Predicates can call typeclass methods; predicate is an ordinary Turmeric expression |
| Algebraic effects | Predicates are pure (`#fx{}` enforced); effects inside predicates are not permitted |
| `any` type | Contract types enable gradual typing for `any` values |
| FFI (`extern-c`) | `:pre` / `:post` validated at the Turmeric/C boundary |

---

## Contract Types vs. Contract Macros

Use **contract macros** when:
- Adding a one-off sanity check inside a function body
- The check is only relevant in a specific branch, not at all entry points
- You want imperative control over exactly where the check runs

Use **contract types** when:
- The predicate is part of the type's meaning (e.g. "a non-negative integer")
- You want the compiler to enforce the check at every call site automatically
- Writing a library API where callers should be told the invariant upfront
- Validating at FFI boundaries

---

## Implementation Phases

Contract types are planned in four phases:

| Phase | Goal |
|---|---|
| CT0 | Parse `#refine{ x : T \| p }` syntax; `TY_CONTRACT` type node; store `:pre`/`:post`/`:contract` on `FnDef` |
| CT1 | Insert runtime checks at function entry, exit, let bindings, and `extern-c` boundaries |
| CT2 | Propagate and simplify contracts through operations; subtype relation `#refine{ x : T \| p } <: T` |
| CT3 | Eliminate provably-true checks at compile time; debug/release build switch |
| CT4 | FFI contracts; `(cast x : T)` downcast from `any`; contract handler API; `tur explain` entries |

---

## See Also

- [Error Handling Guide](error-handling-guide.md) -- `Result`, `Option`, `panic`, and today's contract macros
- [C Integration Guide](c-integration-guide.md) -- FFI and `extern-c`
- [Type Annotations Guide](type-annotations-guide.md) -- compound type syntax
- [Reader Forms Guide](reader-forms-guide.md) -- bare `{...}` curly-infix and the `#refine{...}` data literal
- [Advanced Type System Rationale](advanced-type-system-rationale.md) -- why contract types shipped and refinement types were deferred
