---
title: typeclass method with struct-by-value arg miscompiles when the body grows past a small closure threshold
category: Codegen bug
severity: Medium. Blocks any typeclass instance whose method takes a struct argument and whose body has enough nested calls/`unsafe` blocks to trigger closure conversion. Surfaced while implementing the `derive-json` macro for P2a (typeclass-based JSON encoding); a 2-field defstruct works, a 3-field defstruct miscompiles.
description: An `Encode` typeclass with method `(encode [x] : cstr)` instantiated for a `defstruct User`. The 2-field body `(unsafe (__json-obj-build (cons "id" (cons (encode (.id x)) (cons "name" (cons (encode (.name x)) 0))))))` compiles cleanly. Adding a third field collapses the codegen: the generated C declares `x` as `const User * x` inside the closure env (`__henv_NN->x = x`) but the typeclass method's `__inst_Encode_encode_User` signature delivers `x` by value (`const User x`), and the body still indexes via `->id` / `->name` / `->active`. Output: `error: member reference type 'User' is not a pointer; did you mean to use '.'?`.
status: RESOLVED 2026-06-12 (same day as filing). The env-struct emit in `src/compiler/emit_effects.c` now matches the parameter's pass-by-pointer shape for parameter-captured bindings: when `type_struct_pass_by_ptr(b->type)` AND the binding is in the current `ctx->fn_params`, the env field is emitted as `const T *` rather than `T`, so the env fill `__henv_NN->x = x;` and writeback `x = __henv_NN->x;` are pointer-to-pointer (compile cleanly), and the body's captured-binding access `(__henv_NN->x)->id` is consistent with the pass-by-ptr field-access lowering. Let-bound captures of struct values are unchanged (env still stores them by value). Regression fixture: `tests/fixtures/typeclass-unsafe-passbyptr-struct-arg/`. Suite: 1559 passed / 82 failed (+1 vs prior baseline). The trigger threshold matched Phase D's struct-size rule (>16 bytes): 2-field int structs (16 bytes) were below the threshold and accidentally worked.
---

# typeclass instance miscompiles struct-by-value arg under closure conversion

## Summary

`(definstance Encode [User] (encode [x] (...)))` where `User` is a
`defstruct` and the body has 3+ nested function calls in a single
expression triggers a closure capture of `x`. The capture stores `x` as
a `const User *`, but the typeclass dispatch shim still passes the
struct by value (`const User x`), and the body's field accessors use
the pointer form. Resulting C does not compile.

## Repro

`spices/json/src/json/encode.tur` in the sibling repo (P2a minimal
slice) defines:

```turmeric
(defclass Encode [a] (encode [x] : cstr))

(defn __json-obj-build [lst : int] #{Unsafe} : cstr
  ```c ... walks a cons-list of alternating (key, fragment) cstrs ... ```)
```

Then a hand-written 3-field instance:

```turmeric
(defstruct User [id : int  name : cstr  active : bool])

(definstance Encode [User]
  (encode [x] (unsafe (__json-obj-build
    (cons "id"     (cons (encode (.id x))
    (cons "name"   (cons (encode (.name x))
    (cons "active" (cons (encode (.active x))
    0))))))))))
```

Calling `(encode (make-struct User 7 "alice" true))` triggers:

```
error: member reference type 'User' (aka 'struct User') is not a pointer; did you mean to use '.'?
    ...(__henv_20->x)->id...
                       ~~
error: assigning to 'User' from incompatible type 'const User *'
    __henv_20->x = x;
error: assigning to 'const User *' from incompatible type 'User'; take the address with '&'
    x = __henv_20->x;
```

The same body with only 2 fields (no `active`) compiles and runs:

```
{"id":7,"name":"alice"}
```

The 3-field form fails identically whether expanded from a macro or
hand-written, ruling out macro hygiene.

## Observed vs expected

- **Observed**: 3+ nested calls in an instance body cause closure
  conversion to capture the struct arg as a pointer while the dispatch
  shim still passes by value; the body and the closure disagree on the
  type of `x`.
- **Expected**: either both sides use pointer form (closure stores
  `User *` AND the shim takes `User *`) or both sides use value form
  (no closure capture; inline the body or copy `x` into the env).

## Root-cause hypothesis

The closure-conversion pass picks a representation for the captured
variable independently of the method signature. For small bodies the
pass elides the closure (inlining `x` directly), so the mismatch never
surfaces; for larger bodies it emits the env struct with a pointer slot
without telling the typeclass dispatch shim to also use pointer form.

Likely site: closure-env layout selection in
`src/compiler/elab_fns.c` or `src/compiler/emit_module.c`, near the
typeclass-instance method emission. The threshold appears to be around
the number of operands in the method body (2 vs 3 nested calls); not
yet pinned down whether it's expression depth or environment-slot
count.

## Workarounds

1. Keep `Encode` instance bodies for `defstruct` types at ≤2 nested
   cons-chains. The P2a minimal slice ships with this limitation
   documented in the `derive-json` docstring; the fixture
   `spices/json/tests/derive-encode-struct.tur` exercises a 2-field
   `User`.
2. Split the instance body into a fixed-arity helper that receives
   flattened scalar arguments instead of the struct, so the closure
   never captures the struct value. This is what the `derive-json`
   macro could lower to once the underlying bug is fixed.

## Proposed fix

Audit `elab_fns.c` / `emit_module.c` for the case where a typeclass
instance method's parameter is a `defstruct` value and the body
triggers closure conversion. The dispatch shim's calling convention
and the captured-env layout must agree on by-value vs by-pointer.

## Validation when fixed

- `spices/json/tests/derive-encode-struct.tur` should be updated to
  exercise a 3-field `User` (the original target shape) and
  `derive-json User (id int) (name cstr) (active bool)` should
  produce `{"id":7,"name":"alice","active":true}`.
- The "≤2 fields" caveat in the `derive-json` docstring
  (`spices/json/src/json/encode.tur`) should be removed.
