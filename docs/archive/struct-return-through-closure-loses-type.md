# By-value struct results do not survive the type-erased closure ABI

**Status:** RESOLVED. By-value struct (and ADT) results now flow through the
closure / partial-application / first-class-function path with their type
intact. Both repros below run correctly, constructor currying is enabled, and
the full suite is green (`bash tests/run.sh`).

Fix summary (see CURRY-V0/V1/V2 in
`docs/archive/history/struct-return-through-closure-loses-type.md`):

- **CURRY-V0 (elaboration).** A lambda's bare single-symbol return type that
  names a struct/ADT now resolves to that nominal type and is carried on the
  lambda's `fn_type.result_full_type` (`elab_fn`, `src/compiler/elab_fns.c`) --
  it previously degraded to a `TY_TYVAR`. And the three result-type patches in
  `elab_call` (struct / ADT / exists-forall) now fire only on a genuine full
  application (the call result is the carrier kind), not on an under-applied
  call whose result is a closure value -- otherwise a partial application was
  mis-typed as the aggregate (so a stored `f` was even mistaken for a struct
  *name* by CTOR-V0).
- **CURRY-V1 (codegen).** `elab_partial_apply`'s thunk body now carries the full
  struct/ADT result `Type` (with its def) instead of `type_from_kind`, so emit
  lowers the thunk's C return type to the by-value struct rather than the
  def-less `int64_t` carrier. The thunk is invoked through its typed
  `tur_thunk_<R>_..._t` slot, so the by-value result round-trips without boxing.
- **CURRY-V2 (constructor currying).** An under-applied *positional* `(Name a)`
  partial-applies a synthesized, cached `__ctor_<Name>` backing function (body
  `(make-struct Name ...)`); full applications and the keyword form keep the
  direct make-struct fast path; parameterized structs decline currying.

Fixtures: `tests/fixtures/{lambda-returns-struct,struct-returning-fn-as-value,struct-curry-ctor}/`.

---

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
