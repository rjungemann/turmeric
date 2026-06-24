# By-value struct results do not survive the type-erased closure ABI

**Severity:** medium (blocks currying/first-class use of struct-returning functions).

A function whose result is a by-value struct cannot be called *indirectly*
(through a closure, a partial application, or any first-class function value):
the result silently degrades to the `int64_t` carrier, so both the type and
the value are lost. This is independent of struct ergonomics -- it predates
the `struct-ergonomics-plan` work and is why the auto-bound constructor
(CTOR-V0) does **not** offer currying.

## Minimal repro

Indirect call (lambda) loses the struct type at elaboration:

```turmeric
(defstruct Person :copy [name : cstr age : int])
(defn main [] : int
  (let [f (fn [a : int] : Person (make-struct Person "Bob" a))
        p (f 40)]
    (println (.name p))   ; error: no typeclass method found for 'name'
    0))
```

Partial application fails at codegen with a C type error:

```turmeric
(defstruct Person :copy [name : cstr age : int])
(defn mkp [n : cstr a : int] : Person (make-struct Person n a))
(defn main [] : int
  (let [f (mkp "Bob")     ; partial application -> closure
        p (f 40)]         ; cc: "returning type Person but int64_t expected"
    0))
```

## Root cause

The closure / partial-application calling convention is type-erased: every
closure is invoked through a uniform `int64_t (*)(void*, int64_t...)` pointer,
so both arguments and results are forced through the `int64_t` carrier.

- `elab_partial_apply` (src/compiler/elab_call.c) builds the thunk's result
  type via `type_from_kind(result_kind)`, which drops the `StructDef` -- the
  emitted thunk then returns `int64_t` while its body returns the real struct.
- The indirect-call result at an application site (`(f x)`) is likewise typed
  as the carrier, so a subsequent `(.field ...)` cannot resolve the struct.

A by-value struct wider than a register genuinely cannot round-trip through the
`int64_t` carrier; making this work requires boxing the result (heap-allocate,
return the pointer as the carrier, unbox at the call site) the same way
captured/returned aggregates are handled elsewhere.

## Fix directions

1. Box by-value struct (and ADT) results when a function is used indirectly:
   the thunk returns a heap pointer cast to the carrier; the call site casts
   back and dereferences. `:heap` structs (already pointer-shaped) are the
   cheapest first target.
2. Carry the full result `Type` (with `StructDef`) through
   `elab_partial_apply`'s thunk type and through the indirect-call result type
   so `(f x)` keeps the struct type for field access.

Until then, construct struct-returning functions with full application (or
`make-struct` / the auto-bound constructor positional/keyword forms), not via
currying or stored closures.
