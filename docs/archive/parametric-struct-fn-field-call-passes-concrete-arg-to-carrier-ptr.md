# Parametric struct function-field call-through passes a concrete struct arg to an `int64_t` carrier pointer

**Severity:** medium (blocks `(.field obj args)` / `(. obj field args)`
call-through for any *parametric* struct whose field type mentions the
struct's own type parameters; non-parametric fn-fields work fine)

**Status:** RESOLVED. Split out of
`docs/archive/dot-method-call-misroutes-to-typeclass.md` (issue #3 tail) --
the dot *routing* and result-typing are fixed there; this is the remaining,
independent carrier-ABI defect surfaced once routing worked.

## Resolution

Fixed in the fn-field call-through emit (`src/compiler/emit_expr.c`, the
indirect capability-field call branch). `register_fn_ptr_typedef` returns a
typedef for `(fn [S] A)` because the field's kinds are erased to the int64
carrier -- so `is_typed_fn_field` was set, and the emitter took the
direct-call path that passes the concrete `Person` argument straight through a
`tur_fnptr_int64_t_int64_t_t` (int64-parameter) pointer. We now detect that
the field's fn `full_type` is written over type variables (new helper
`fn_field_full_type_mentions_tyvar`, which inspects `arg_full_types` /
`result_full_type` for a `TY_TYVAR`) and, in that case, clear
`is_typed_fn_field`. The call then falls to the intptr_t-cast path, which
specialises the pointer to the call's concrete arg/result C types:

```
puts(((const char * (*)(Person))(intptr_t)((l_1251).get))(p_1250));
```

Both the joined `(.get l p)` and receiver-first `(. l get p)` forms now call
through correctly, and the struct-returning `put` field
(`(.put l p "Alice")`) round-trips. Regression fixture:
`tests/fixtures/dot-parametric-fn-field-call`. Full suite green
(1788 passed, 0 failed).

## Summary

Calling a function-typed field of a **parametric** struct, where the field's
type is written in terms of the struct's type parameters (e.g. `(fn [S] A)`
on `(defstruct Lens [S A] (get (fn [S] A)) ...)`), emits C that passes a
concrete struct value to a function pointer whose parameter was lowered to the
`int64_t` carrier. `cc` then rejects the call with an incompatible-type error.

A **non-parametric** fn-field whose signature is fully concrete
(`(defstruct Box [get (fn [Person] cstr)])`) works correctly -- both the joined
`(.get b p)` and receiver-first `(. b get p)` forms call through and return the
right value. The defect is specific to fields typed over the struct's own type
parameters.

## Repro

```turmeric
(defstruct Lens [S A]
  (get (fn [S] A))
  (put (fn [S A] S)))

(defstruct Person :copy
  [name : cstr age : int])

(defn name-get [p : Person] : cstr
  (.name p))

(defn name-put [p : Person n : cstr] : Person
  (make-struct Person n (.age p)))

(defn main [] : int
  (let [p (make-struct Person "Bob" 40)
        l (make-struct Lens name-get name-put)]
    (println (.get l p))   ; <- same failure via (. l get p)
    0))
```

`cc` error on the emitted translation unit:

```
error: incompatible type for argument 1 of 'l_1251.get'
   puts(((l_1251).get)(p_1250));
                       ^~~~~~
note: expected 'int64_t' {aka 'long int'} but argument is of type 'Person'
```

The field `get : (fn [S] A)` is stored in the C struct as a pointer whose
parameter type is the `int64_t` carrier (because `S`/`A` are type parameters),
but the call site passes the concrete `Person` value `p_1250` without bridging
it to the carrier representation.

## Root cause (direction)

This is a member of the parametric-struct-by-value/carrier family (cf.
`docs/archive/history/parametric-struct-by-value-carrier-inconsistency.md`,
`docs/archive/history/defstruct-bare-user-type-field-reads-back-as-int-carrier.md`).
The struct's function-field slot is lowered with the type parameters erased to
the `int64_t` carrier, but the indirect-call emit at the use site
(`elab_method_call`'s capability-field call-through, `src/compiler/elab_typeclasses.c`)
elaborates each argument at its concrete type and passes it straight through.
Either the argument must be bridged to the carrier at the call boundary
(matching the stored pointer's ABI), or the field's pointer type must be
specialized to the receiver's concrete type arguments at the call site (the
same per-instantiation specialization the typeclass-dispatch path performs via
`abi_bindings`).

## Fix directions

- At the fn-field call-through in `elab_method_call`
  (`src/compiler/elab_typeclasses.c`, the `call->as.list.len > 2` /
  capability-field branch), when the owning struct is parametric and the field
  type mentions the struct's type parameters, bridge each concrete argument to
  the carrier representation the stored pointer expects -- or specialize the
  emitted pointer type to the receiver's concrete `app_args` so the C types
  line up. The receiver's `TY_APP` args are already extracted nearby
  (`elab_struct_type_extract_args`).
- Add a fixture once fixed (a parametric `Lens`/`Person` get/put round-trip)
  alongside the existing non-parametric `tests/fixtures/dot-receiver-first-call`.
