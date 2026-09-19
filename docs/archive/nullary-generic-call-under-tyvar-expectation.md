# A nullary generic call under a tyvar-shaped expectation is rejected against itself

**RESOLVED 2026-09-19** (same day as filed). Pinned by
`tests/fixtures/nullary-generic-call-under-tyvar-expectation` (a user nullary
generic under a user generic's parameter, `(none)` under a ctor's
`(Option A)` field, and a chained `(push 1 (push 2 (push 3 (node-nil))))` at
two element types), on both back ends.

**Severity: low-medium** (expressiveness hole with a one-token workaround).
Found 2026-09-19 while resolving
[self-typed-heap-parametric-field-unsupported](self-typed-heap-parametric-field-unsupported.md);
it reproduces on a non-recursive def, so it is not that shape's.

## Repro

```turmeric
(defstruct W :heap [A] (val A) (opt (Option A)))
(defn main [] : int
  (let [w (make-struct W 8 (none))]
    (println (.val w)))
  0)
;; error [TUR-E0001]: function 'W' arg 2:
;;   expected (type-app Option tyvar 'A'), got (type-app Option tyvar 'A')
```

The same with a user nullary generic:

```turmeric
(defstruct Box :heap [A] (val A))
(defn box-nil [A] [] : (Box A) (:: 0 (Box A)))
(defn wrap [A] [v : A b : (Box A)] : (Box A) (make-struct Box v))
(wrap 3 (box-nil))
;; function 'wrap' arg 2: expected (type-app Box tyvar 'A'), got (type-app Box tyvar 'A')
```

Both back ends rejected it identically (the check is in elaboration).

## Root cause

Exactly as guessed: `call_collect_type_bindings` (elab_call.c) had already
bound the callee's `A := int` from the sibling argument, and on reaching the
`(Box 'A)` argument compared that binding with the argument's OWN open
tyvar -- the nullary call's result, whose `A` nothing had grounded -- with
`type_eq`, which is false for `int` vs a tyvar.  A saturated argument
(`(some 3)`) grounds its `A` first and never hit it.

## Resolution

In the tyvar-carrying-parameter branch of `elab_call_fn_inner`, when the
collect fails and the argument is a return-only polymorphic call result
(W2's `w2_arg_is_free_poly_call` population), the parameter is instantiated
with the bindings the siblings produced (`call_instantiate_type`) and, when
that grounds it, the argument's tyvar is unified against the grounded type
exactly as W2 does for a concrete parameter: the substitution is recorded on
the nullary call (`abi_bindings`, so emit monomorphizes it --
`none__spec__Option__int`) and the grounded type becomes the argument's.  A
parameter the siblings leave open keeps the ordinary rejection.

This is what let `stdlib/list.tur`'s `tnil` become `[A] [] : (Cons A)` for
M7 of the Saffron surface pass: `(tcons-of 1 (tnil))` is this shape.
