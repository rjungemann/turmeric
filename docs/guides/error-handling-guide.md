---
title: Error Handling Guide
category: Error Handling
description: `Result`, `Option`, `panic`, contract macros (`assert!`, `require!`, `ensure!`)
---

# Error Handling Guide

This guide covers the Turmeric error handling story: `Result`, `Option`, `panic`,
unwrap helpers, and contract macros. It describes what is implemented today.

## Overview

Turmeric offers two primary error handling strategies:

1. **`Result`** -- recoverable errors; use when callers should handle failure.
2. **`panic`** -- unrecoverable errors; use for programming mistakes and invariant violations.

`Option` is a separate type for nullable / optional values and has its own unwrap helpers.
Contract macros (`assert!`, `require!`, `ensure!`, `invariant!`) provide structured
precondition and postcondition checking built on top of `panic`.

---

## `Result`

A result value is either `(ok value)` or `(err error)`. The runtime representation
is a heap-allocated struct `{ bool is_ok; int64_t ok_val; int64_t err_val; }` returned
as `ptr<void>`. Both the ok and err fields are `int64_t` in v1; typed generics are a
future follow-on.

### Constructors

```turmeric
(ok 42)        ;; ok result
(err 99)       ;; err result
```
```sweet-exp
ok(42)         ;; ok result
err(99)        ;; err result
```

### Predicates

```turmeric
(ok?  r)       ;; => bool
(err? r)       ;; => bool
```
```sweet-exp
ok?(r)         ;; => bool
err?(r)        ;; => bool
```

### Extractors

These are unsafe -- check `ok?` / `err?` first:

```turmeric
(ok-val  r)    ;; => int  (undefined behaviour if r is err)
(err-val r)    ;; => int  (undefined behaviour if r is ok)
```
```sweet-exp
ok-val(r)      ;; => int  (undefined behaviour if r is err)
err-val(r)     ;; => int  (undefined behaviour if r is ok)
```

### Unwrapping

```turmeric
(result-unwrap     r)            ;; ok value, or abort() with message to stderr
(result-unwrap-or  r default)    ;; ok value, or default
(result-expect     r "message")  ;; ok value, or abort() printing "message"
(result-must       r)            ;; ok value, or panic "result-must: called on err"
(result-must-msg   r "msg")      ;; ok value, or panic with "msg"
```
```sweet-exp
result-unwrap(r)               ;; ok value, or abort() with message to stderr
result-unwrap-or(r default)    ;; ok value, or default
result-expect(r "message")     ;; ok value, or abort() printing "message"
result-must(r)                 ;; ok value, or panic "result-must: called on err"
result-must-msg(r "msg")       ;; ok value, or panic with "msg"
```

> **Note:** `result-unwrap` and `result-expect` call `abort()` directly (not via
> `tur_panic`). Prefer `result-must` / `result-must-msg` when you want the standard
> panic message format and double-panic guard.

### Combinators

```turmeric
(result-map      r f)            ;; apply f to ok value; propagate err unchanged
(result-map-err  r f)            ;; apply f to err value; propagate ok unchanged
(result-flat-map r f)            ;; f receives ok value and returns a new result
(result-or       r alt)          ;; r if ok, else alt
(result-or-else  r f)            ;; r if ok, else (f err-value)
```
```sweet-exp
result-map(r f)                ;; apply f to ok value; propagate err unchanged
result-map-err(r f)            ;; apply f to err value; propagate ok unchanged
result-flat-map(r f)           ;; f receives ok value and returns a new result
result-or(r alt)               ;; r if ok, else alt
result-or-else(r f)            ;; r if ok, else f(err-value)
```

### Equality

```turmeric
(result-eq? r1 r2 ok-cmp err-cmp)
;; ok-cmp  -- fn [a b :int] :bool  used when both are ok
;; err-cmp -- fn [a b :int] :bool  used when both are err
;; => bool; false if variants differ
```
```sweet-exp
result-eq?(r1 r2 ok-cmp err-cmp)
;; ok-cmp  -- fn [a b :int] :bool  used when both are ok
;; err-cmp -- fn [a b :int] :bool  used when both are err
;; => bool; false if variants differ
```

The `Eq` typeclass instance uses `=` for both sides:

```turmeric
(eq? (ok 1) (ok 1))   ;; => true
(eq? (ok 1) (err 1))  ;; => false
```
```sweet-exp
eq?(ok(1) ok(1))    ;; => true
eq?(ok(1) err(1))   ;; => false
```

### Option interop

```turmeric
(ok-or   opt err-val)    ;; some(v) -> ok(v), none -> err(err-val)
(err-context r "prefix") ;; prepend "prefix: " to the err string; ok passes through
```
```sweet-exp
ok-or(opt err-val)       ;; some(v) -> ok(v), none -> err(err-val)
err-context(r "prefix")  ;; prepend "prefix: " to the err string; ok passes through
```

### Collection utilities

```turmeric
(result-collect    vec)    ;; (vec result) -> result<vec, E>; first err wins
(result-partition  vec)    ;; -> pair; separate ok and err elements
(result-partition-ok  pair)  ;; -> vec of ok values from the pair
(result-partition-err pair)  ;; -> vec of err values from the pair
```
```sweet-exp
result-collect(vec)          ;; (vec result) -> result<vec, E>; first err wins
result-partition(vec)        ;; -> pair; separate ok and err elements
result-partition-ok(pair)    ;; -> vec of ok values from the pair
result-partition-err(pair)   ;; -> vec of err values from the pair
```

### Memory

```turmeric
(result-free r)    ;; free the heap struct (does not free the contained value)
```
```sweet-exp
result-free(r)     ;; free the heap struct (does not free the contained value)
```

### Typeclass instances

`result` (as `ptr<void>`) implements `Functor`, `Applicative`, `Monad`,
`Foldable`, `Bifunctor`, and `Eq`. These are defined in `stdlib/result.tur`.

| Typeclass    | Behaviour on `ok` / `err` |
|---|---|
| `Functor`    | `fmap` applies to ok value; err propagates |
| `Applicative`| `pure x = ok(x)`; `ap` short-circuits on first err |
| `Monad`      | `bind` applies f to ok value; err propagates |
| `Foldable`   | `foldl` / `foldr` operate on ok value; err is skipped |
| `Bifunctor`  | `bimap fn-left fn-right` -- fn-left over err, fn-right over ok |
| `Eq`         | structural equality via `result-eq?` with `=` comparator |

---

## Query operator: `?`

The `?` operator unwraps an ok `Result` in place, or returns the err `Result`
early from the enclosing function. It is the ergonomic counterpart to manual
`(if (err? r) (return r) (ok-val r))` threading.

```turmeric
(? expr)            ;; s-expression
```
```sweet-exp
?(expr)             ;; neoteric -- composes inside other calls
(? expr)            ;; traditional spelling also works under #lang sweet-exp
```

### Lowering

`(? expr)` lowers to a single-evaluation `let` so `expr` runs exactly once:

```turmeric
(let [__q expr]
  (if (err? __q)
      (return __q)        ;; propagate the err Result unchanged
      (ok-val __q)))      ;; otherwise yield the unwrapped ok value
```

The lowering routes through the `#{}`-safe stdlib helpers `__tur-q-is-err?`
and `__tur-q-ok-val` (in `stdlib/result.tur`), so call sites need no `(unsafe
...)` wrapper.

### Scope rule

`?` is only valid **inside a function body** -- using it at the top level is a
hard error (`? operator is only allowed inside a function body`). Because it
expands to `(return <err>)`, the enclosing function must itself return a
`Result` so the propagated err is well-typed.

Applying `?` to a non-`Result` literal is rejected up front:

```turmeric
(? 5)   ;; error [TUR-E0001]: ? operator requires a Result value, got int
```

Computed non-`Result` operands are caught by the same `TUR-E0001` check when
the `__tur-q-is-err?` helper fails to accept them.

### Worked example

```turmeric
;; parse-config threads three fallible steps; any err short-circuits.
(defn parse-config [src :ptr<void>] :ptr<void>
  (let [raw    (? (read-source  src))]
    (let [toks (? (tokenize     raw))]
      (let [ast (? (parse-forms toks))]
        (ok ast)))))
```

If `read-source`, `tokenize`, or `parse-forms` returns an err, `parse-config`
returns that err immediately; otherwise it returns `(ok ast)`.

For the "transform the error, then propagate" use case, combine `?` with
[`result-or-else`](#combinators) to rewrite the err payload before it bubbles
up.

---

## `Option`

An option is either `(some value)` or `(none)`. `none` is represented as `NULL`.

### Constructors

```turmeric
(some 42)    ;; some option
(none)       ;; none option (NULL)
```
```sweet-exp
some(42)     ;; some option
none()       ;; none option (NULL)
```

### Predicates

```turmeric
(some? o)    ;; => bool
```
```sweet-exp
some?(o)     ;; => bool
```

### Unwrapping

```turmeric
(option-unwrap o)            ;; value, or exit(1) with message to stderr
(option-must   o)            ;; value, or panic "option-must: called on none"
(option-expect o "message")  ;; value, or panic with "message"
```
```sweet-exp
option-unwrap(o)             ;; value, or exit(1) with message to stderr
option-must(o)               ;; value, or panic "option-must: called on none"
option-expect(o "message")   ;; value, or panic with "message"
```

> **Note:** `option-unwrap` calls `exit(1)` directly. Prefer `option-must` /
> `option-expect` for consistent panic semantics.

### Option interop with Result

```turmeric
(ok-or opt err-val)    ;; some(v) -> ok(v), none -> err(err-val)
```
```sweet-exp
ok-or(opt err-val)     ;; some(v) -> ok(v), none -> err(err-val)
```

### Equality

```turmeric
(option-eq? o1 o2 cmp-fn)
;; cmp-fn -- fn [a b :int] :bool
;; => true if both none, or both some with cmp-fn returning true
```
```sweet-exp
option-eq?(o1 o2 cmp-fn)
;; cmp-fn -- fn [a b :int] :bool
;; => true if both none, or both some with cmp-fn returning true
```

The `Eq` typeclass instance uses `=` for the contained value.

### Memory

```turmeric
(option-free o)    ;; free the heap struct
```
```sweet-exp
option-free(o)     ;; free the heap struct
```

### Typeclass instances

`option` implements `Functor`, `Applicative`, `Monad`, `Foldable`, `Traversable`,
`Alternative`, `Eq`, `Clone`, and `Show`. These are defined in `stdlib/option.tur`.

---

## `panic`

`panic` terminates the program unconditionally. Use it for:

- Bugs and invariant violations that cannot be recovered from.
- Assertions in test code.
- Unreachable code branches.

```turmeric
(panic "something went wrong")
```
```sweet-exp
panic("something went wrong")
```

This prints `panic: something went wrong` to stderr and calls `abort()`.

### Double-panic guard

If `tur_panic` is called while a panic is already in progress (e.g. in a `defer`
chain), it prints `double panic: aborting` and calls `abort()` immediately.

### `defer` during panic

Defer thunks registered before the panic are fired in reverse order during
unwinding. If a defer thunk itself panics, the double-panic guard triggers `abort()`.

---

## Unwrap helpers: `must!` and `must-msg!`

These macros unwrap an **option** value, panicking on `none`:

```turmeric
(must!     (some 42))                   ;; => 42
(must!     (none))                      ;; panic: option-must: called on none

(must-msg! (some 42) "expected value")  ;; => 42
(must-msg! (none)    "expected value")  ;; panic: expected value
```
```sweet-exp
must!(some(42))                         ;; => 42
must!(none())                           ;; panic: option-must: called on none

must-msg!(some(42) "expected value")    ;; => 42
must-msg!(none()   "expected value")    ;; panic: expected value
```

> `must!` expands to `(option-must expr)`. `must-msg!` expands to
> `(option-expect expr msg)`. For `result` values use `result-must` /
> `result-must-msg` directly -- the macros do not dispatch generically.

---

## `option-must` and `option-expect`

These are the underlying functions used by `must!` and `must-msg!`:

```turmeric
(option-must   (some 42))               ;; => 42
(option-must   (none))                  ;; panic: option-must: called on none
(option-expect (some 42) "want value")  ;; => 42
(option-expect (none)    "want value")  ;; panic: want value
```
```sweet-exp
option-must $ some(42)               ;; => 42
option-must $ none()                 ;; panic: option-must: called on none
option-expect(some(42) "want value") ;; => 42
option-expect(none()   "want value") ;; panic: want value
```

---

## `ignore!`

Explicitly discard a result or option value to silence unused-value warnings
(when linting is enabled):

```turmeric
(ignore! (some-fn-returning-result))
```
```sweet-exp
ignore!(some-fn-returning-result())
```

`ignore!` expands to `(do expr nil)` -- the expression is evaluated for its side
effects and the value is dropped.

### Linting unused results

Pass `--warn-unused-result` to have the compiler warn when a `result`-shaped
value (a `ptr<void>`) is computed in statement position and its value is thrown
away:

```turmeric
(defn main [] :int
  (write-record r)   ;; warning: discarded result value of type ptr<void>;
                     ;;          use ignore! to suppress this warning
  0)
```

The warning is **off by default** and is silenced three ways:

- **`ignore!`** -- `(ignore! (write-record r))` expands to `(do expr nil)`, so
  the discarded value is `nil`, not a result. Use this when discarding is
  intentional.
- **Binding it** -- `(let [_ (write-record r)] ...)`. A `let` binding (even to
  `_`) is an explicit discard and never warns.
- **Using the value** -- e.g. propagating it with the [`?` operator](#query-operator-).
  Because `(? expr)` binds its operand in a `let`, a `?`-wrapped call is never
  in statement position and never triggers the warning.

---

## Contract macros

Contract macros live in `stdlib/macros.tur` (module `tur/macros`) and are
auto-imported. They all expand to `tur-contract-check` or `tur-contract-check-inv`
from `stdlib/contract.tur`, which call `tur_panic` on failure.

By default contracts are enabled (`contract-enabled?` returns `true`). Pass
`--no-contracts` to strip them from a release build: every `assert!` /
`require!` / `ensure!` / `invariant!` is dropped **at elaboration time**, so the
contract's predicate expression is never evaluated, and `contract-enabled?`
folds to `false`.

> **Side-effect warning.** Because the predicate is removed entirely, any side
> effect inside a contract condition disappears under `--no-contracts` -- this
> matches the C / Rust `assert` convention. Keep contract predicates pure;
> never rely on a side effect that lives inside `(assert! ...)`.

The codegen preamble also defines `TUR_CONTRACTS_ENABLED` (`1` normally, `0`
under `--no-contracts`) so inline-C blocks can branch on the build mode. See
the [compiler flags guide](compiler-flags-guide.md) for the release-build
recipe.

### `assert!` and `assert-msg!`

Unconditional sanity check. Use anywhere you want to verify an intermediate
condition holds:

```turmeric
(assert! (= x 1))
;; panics with "Assertion failed" if x != 1

(assert-msg! (= x 1) "x must be 1")
;; panics with "x must be 1" if x != 1
```
```sweet-exp
assert!({x = 1})
;; panics with "Assertion failed" if x != 1

assert-msg!({x = 1} "x must be 1")
;; panics with "x must be 1" if x != 1
```

### `require!` and `require-msg!`

Precondition check at function entry:

```turmeric
(defn sqrt [n :int] :int
  (require! (>= n 0))
  ...)
;; panics with "Precondition failed" if n < 0

(defn sqrt [n :int] :int
  (require-msg! (>= n 0) "sqrt: n must be non-negative")
  ...)
```
```sweet-exp
defn sqrt [n :int] :int
  require!({n >= 0})
  ...
;; panics with "Precondition failed" if n < 0

defn sqrt [n :int] :int
  require-msg!({n >= 0} "sqrt: n must be non-negative")
  ...
```

### `ensure!` and `ensure-msg!`

Postcondition check before returning from a function:

```turmeric
(defn abs [n :int] :int
  (let [result (if (< n 0) (- 0 n) n)]
    (ensure! (>= result 0))
    result))
;; panics with "Postcondition failed" if the result is somehow negative
```
```sweet-exp
defn abs [n :int] :int
  let [result
       if {n < 0}
         {0 - n}
         n]
    ensure!({result >= 0})
    result
;; panics with "Postcondition failed" if the result is somehow negative
```

### `invariant!` and `invariant-msg!`

Structural invariant check -- passes the value to a predicate function:

```turmeric
(invariant! my-list non-empty?)
;; panics with "Invariant failed" if (non-empty? my-list) is false

(invariant-msg! my-list non-empty? "list must not be empty")
```
```sweet-exp
invariant!(my-list non-empty?)
;; panics with "Invariant failed" if non-empty?(my-list) is false

invariant-msg!(my-list non-empty? "list must not be empty")
```

---

## When to use what

| Situation | Approach |
|---|---|
| Caller should handle the failure (e.g. file not found) | `result` |
| Value may or may not be present | `option` / `some` / `none` |
| Programming error / violated invariant | `panic` |
| Unwrapping a result you are confident is ok | `result-must` / `result-must-msg` |
| Unwrapping an option you are confident is some | `option-must` / `must!` |
| Checking a precondition at function entry | `require!` / `require-msg!` |
| Checking a postcondition before returning | `ensure!` / `ensure-msg!` |
| Verifying a structural invariant | `invariant!` / `invariant-msg!` |
| General sanity check in the middle of code | `assert!` / `assert-msg!` |
| Discarding a result intentionally | `ignore!` |

---

## Deferred

The following features are planned but not yet implemented:

| Feature | Phase | Notes |
|---|---|---|
| `catch-unwind` | R2 | Catch a panic at a safe boundary |
| `--lint-panic` compiler flag | R6 | Audit panic call sites |

### Panic inside effect handlers and continuations (Phase R6)

When a panic occurs inside an effect handler or continuation:

1. **Effect handlers**: Panics propagate normally through the handler chain unless
   a `catch-unwind` boundary (Phase R2) is present.
2. **Continuations**: A panic unwinds the stack to the `reset` boundary. Resuming
   a continuation (`shift`/`reset`) after a panic clears the panic state; it does
   not re-panic automatically.
3. **Defer**: Defer thunks fire during panic unwinding. A defer thunk that itself
   panics triggers the double-panic guard and calls `abort()`.

### Panic inside async tasks (Phase R6)

In v1, `(async fn)` calls the function synchronously with no fiber scheduler. A
panic inside an async body propagates through the call stack and is not caught at
any task boundary.

The v2 target behavior (when true fiber-based async lands):

1. A panic inside an async task is caught at the task boundary. The task's future
   resolves to a rejected state carrying the panic payload; use `catch-unwind` at
   the join point to recover.
2. If a task is cancelled while a panic is in progress, the panic takes precedence.
3. An uncaught panic in async main terminates the process with a nonzero exit code
   after all defer thunks have fired.
4. WASM target: panics lower to the WebAssembly `unreachable` instruction.
