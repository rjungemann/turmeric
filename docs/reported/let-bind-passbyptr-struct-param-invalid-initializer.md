# `let`-binding a by-pointer struct parameter emits an invalid C initializer

**Severity:** low (narrow; pre-existing, independent of struct/ADT convergence)

## Summary

Binding a struct-typed *parameter* that uses the by-pointer ABI (its fields sum
to > 16 bytes, so it is passed as `const T *`) directly to a `let` variable
generates `T g = t;` in C, where `t` is a pointer -- a type mismatch that `cc`
rejects with "invalid initializer".

## Minimal repro

```turmeric
(defstruct Triple :copy [a : int b : int c : int])   ; 24 bytes -> by-pointer ABI
(defn f [t : Triple] : int
  (let [g t]
    (.a g)))
(defn main [] : int
  (println (f (make-struct Triple 1 2 3)))
  0)
```

```
/tmp/tur-build/_tmp_pbp_tur.c: In function 'f':
  Triple g_1246 = t;          // t is `const Triple *`
                  ^
tur: cc invocation failed (status 256)
```

A by-*value* struct param (<= 16 bytes, e.g. `[x : int y : int]`) binds fine;
only the by-pointer ABI case is broken.

## Root cause (direction)

The `let` binder copies the initializer by value (`T g = <init>;`) without
accounting for the receiver arriving as `const T *` under the by-pointer struct
ABI. The let-binding codegen should dereference (`T g = *t;`) -- or bind a
pointer alias -- when the initializer expression is a by-pointer struct lvalue.
See the struct field-access codegen in `src/compiler/emit_expr.c` (EX_GET_FIELD)
for how the by-pointer receiver is already handled there.

## Workaround in the wild

CONV-S3 (`match` on a struct value) avoids this by reading fields directly off
a bare-variable scrutinee rather than copying it into a temp; only a *compound*
scrutinee uses a temp, and only a > 16-byte compound scrutinee would hit this.
