---
title: Constrained parametric `definstance` body mis-dispatches a class method on a pointer-carried element type
category: Typeclass dispatch / emit -- silent miscompile
severity: Medium-high. SILENT miscompile, not a hard error (for scalar
  elements). A constrained parametric instance body that dispatches a class
  method on its element type (e.g. `(definstance Enc [Option] [(Enc A)] (enc [x]
  (enc (.value x))))`) baked in the int64-carrier instance (`enc_int`) for
  EVERY element type. Correct for `(Option int)`; for `(Option cstr)` /
  `(Option float)` it emitted a raw pointer / reinterpreted value instead of the
  encoded result, and for `(Option SomeStruct)` it failed to compile (aggregate
  passed where the carrier int was expected). This is exactly the class of bug
  CLAUDE.md flags -- "works by luck because the register classes happen to
  match" / silent miscompile.
status: RESOLVED -- 2026-06-20 (see "Resolution" below). cstr/float/int and
  value-struct elements all dispatch and lower correctly; new fixture
  `tests/fixtures/constrained-instance-element-dispatch` locks it in.
  Discovered while implementing JSON `Encode [Option]` in turmeric-spices (U2).
---

# Constrained parametric `definstance` body mis-dispatches on a pointer-carried element

## One-line summary

In the body of `(definstance Enc [Option] [(Enc A)] ...)`, the inner call
`(enc (.value x))` always resolved to `__inst_Enc_enc_int` regardless of the
concrete element type `A`, because the emit-side method re-resolver only
re-dispatched on a bare `TY_TYVAR` receiver and the element extracted via
`(.value x)` had been erased to the int64 carrier at elaboration.

## Minimal repro

```turmeric
(defclass Enc [a] (enc [x] : cstr))

(definstance Enc [int]
  (enc [x] : cstr
    ```c
    char *buf = (char *)malloc(32);
    snprintf(buf, 32, "%lld", (long long)x); return buf;
    ```))

(definstance Enc [cstr]
  (enc [x] : cstr
    ```c
    size_t n = strlen((const char *)x) + 3;
    char *buf = (char *)malloc(n);
    snprintf(buf, n, "\"%s\"", (const char *)x); return buf;
    ```))

(definstance Enc [Option]
  [(Enc A)]
  (enc [x] : cstr
    (if (.is-some x) (enc (.value x)) "null")))

(defn main [] : int
  (println (enc (some 42)))    ; want: 42
  (println (enc (some "hi")))  ; want: "hi"  -- got a raw pointer before the fix
  0)
```

A plain constrained `defn` dispatched fine: `(defn enc-via [A] [(Enc A)] [x : A]
: cstr (enc x))` -- its `cstr` specialization correctly called
`__inst_Enc_enc_cstr`. Only the constrained-*instance* method body misdispatched.

## Observed vs expected (codegen)

The `cstr` specialization of the Option instance emitted:

```c
static const char * __inst_Enc_enc_Option__spec__const_char___Option__cstr(Option__cstr x) {
    ...
    __t45 = __inst_Enc_enc_int((x).value);   // WRONG: enc_int, not enc_cstr
```

Expected (post-fix):

```c
    __t45 = __inst_Enc_enc_cstr((x).value);  // dispatches on the element type
```

## Root cause

Constrained generics are realized by emit-time ABI specialization. When a class
method is dispatched on a constraint type variable, the call is tagged with a
`dict_arg` (an `EX_DICT` recording the instance + method) and
`emit_core.c:emit_reresolve_method_call` re-dispatches it to
`__inst_<Class>_<method>_<T>` per specialization.

That re-resolver derived the "dispatch type" only from:
1. arg 0 (the receiver), **when its elaborated type is `TY_TYVAR`**, or
2. the call's result type, when that is `TY_TYVAR`.

For `(enc (.value x))` the receiver is an `EX_GET_FIELD` whose declared field
type is the struct type-param `A`, but elaboration summarizes that field as the
int64 **carrier** (`TY_INT`), not `TY_TYVAR`. So neither branch fired,
`have_disp` stayed false, and the call fell through to the carrier base clone's
`__inst_Enc_enc_int`. (`enc-via` worked because its receiver `x : A` is a genuine
`TY_TYVAR` parameter, so branch 1 fired.)

A second, downstream instance of the same root cause: the value-struct field
deref in `emit_expr.c` (which stores an `Option`/`Result` value-struct payload
as a heap pointer) called `emit_resolve_type` on the receiver, which leaves a
spec parameter's parametric type (`(Option A)`) unsubstituted -- so it never saw
the concrete element and skipped the deref, producing an ABI mismatch for
struct elements.

## Fix

Two surgical changes, both keyed on "recover the concrete element type from the
current spec's `arg_types[]` when the receiver is a spec parameter" (the same
recovery `emit_var_spec_arg_type` / Path A.2 already use):

- `src/compiler/emit_core.c` (`emit_reresolve_method_call`): when no dispatch
  tyvar is found and the receiver is `(.field container)`, resolve the
  container's concrete type for this spec (spec arg type for a param receiver,
  else `emit_resolve_type`). If it is a parametric app, substitute the extracted
  element args into the field's declared type-param; if it is the monomorphized
  container struct (e.g. `Option__cstr`), read the field's concrete declared
  type. Dispatch on that recovered element type.

- `src/compiler/emit_expr.c` (the `field_is_heap_ptr_for_value_struct` block):
  prefer the spec arg type via `emit_var_spec_arg_type` so the value-struct field
  deref fires for monomorphized containers; handle the monomorphized-struct
  receiver shape directly.

The carrier base clone is unaffected (no spec context => no re-resolution =>
keeps `enc_int`), so the uniform-carrier dispatch path is unchanged.

## Tests

- New fixture: `tests/fixtures/constrained-instance-element-dispatch` --
  `Enc [Option]` over int / float / cstr / value-struct elements; prints
  `42`, `3.25`, `"hi"`, `99`.
- Full suite green: `bash tests/run.sh` => `1729 passed, 0 failed`.
