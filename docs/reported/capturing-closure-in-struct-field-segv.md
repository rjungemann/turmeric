# Capturing closure stored in a struct fn-field is called as a thin fn pointer (SEGV)

**Severity: high** -- a miscompile/SEGV (not a leak) on a natural program:
store a capturing lambda in a `defstruct` function-typed field, read it back, and
call it. General codegen, no effects/CPS needed. Discovered while scoping S2
(closure drop glue for STORED closures) in
[docs/upcoming/closure-drop-glue-plan.md](../upcoming/closure-drop-glue-plan.md);
it is the concrete blocker for that slice -- a stored capturing closure does not
even *run*, so freeing it is moot until this is fixed.

## Minimal repro (no effects)

```turmeric
(defstruct Adder [f : (fn [int] #fx{} int)])
(defn main [] : int
  (let [k    10
        box  (make-struct Adder (fn [x : int] : int (+ x k)))   ; capturing -> fat
        r    (.f box)]
    (r 5)))                                                     ; SEGV
```

`tur build` + run: **SEGV**. A NON-capturing value in the same field works:

```turmeric
(defn add1 [x : int] : int (+ x 1))
(defstruct Adder [f : (fn [int] #fx{} int)])
(defn main [] : int (let [box (make-struct Adder add1)] ((.f box) 5)))  ; OK -> 6
```

So the trigger is specifically a **capturing (fat) closure** stored in the field.

## Root cause

A capturing closure is a *fat* value: a heap env whose slot 0 is the thunk, called
as `thunk(env, args)`. Stored into the struct, only the **env pointer** lands in
the field:

```c
struct __env_1285 *__t154 = malloc(sizeof(struct __env_1285));
__t154->__fn = (tur_thunk_int64_t_int64_t_t)__fn_1283;   /* slot 0 = thunk */
__t154->k    = k_1281;
ctor_Adder((int64_t)(intptr_t)__t154);                   /* field .f = env ptr */
```

But the field read + call emits a **thin function-pointer** call -- it casts the
stored env pointer straight to `R (*)(A)` and calls it with NO env argument, i.e.
it executes the env struct as code:

```c
int64_t (*r)(int64_t) = (int64_t (*)(int64_t))(intptr_t)((int64_t)(box_1288).f);
(((int64_t (*)(int64_t))(intptr_t)r)(INT64_C(5)));       /* calls the ENV as a fn -> SEGV */
```

Contrast the SAME closure bound to a `let` and called directly, which the emitter
dispatches correctly as fat (`__fn_1281(env, 5)`) -- so the fat-ness is known for a
local binding but LOST once the value is read from a struct field.

The dispatch decision keys on `type.as.fn.boxed` / `Binding.is_fat`
(`src/compiler/emit_expr.c:1153`, `:1470`). A struct fn-field's declared type is a
plain non-boxed `(fn [int] int)`, and the binding produced by `(.f box)` is not
`is_fat`, so the call site takes the thin-fn-pointer path. Nothing marks the
field-read value as a fat carrier.

## Why the field cannot just "always fat-dispatch"

The same field legitimately holds a THIN top-level fn (the `add1` case above),
which the thin path calls correctly. A single field type carries either
representation across different stores, and the read site cannot tell which was
stored. So the fix is not a local call-site tweak.

## Fix direction (representation-unification -- the S2 Phase-0 prerequisite)

Make a struct/ADT fn-typed field use the **fat** representation uniformly:

1. Mark the field's `(fn ...)` type `boxed` (fat) so every read/call of `(.field
   v)` dispatches through the `{thunk, env}` protocol (slot-0 thunk + env), the
   same path a `^fat` param uses.
2. Auto-shim a THIN fn stored into the field into a fat handle at the store site
   (a non-capturing fn becomes a `{thunk, env=NULL}` box), exactly as the `^fat`
   parameter shim already does for arguments.

This is the "closure-representation-unification (Phase 0)" the closure-drop-glue
plan names; once a struct fn-field is uniformly fat, S2 can (a) call stored
closures correctly and (b) attach `drop_glue_env_N` to the holding struct's drop
glue to free the env. Until then, `hkt-stdlib-parser-instances` / httpd middleware
(closures stored in `Parser` / chain nodes) stay on the carrier/eviction path.

## Note

This is orthogonal to the fat-closure ENV LEAK work already landed (S1: freeing a
fresh/inline closure consumed by a non-retaining callee). That path fixed *freeing*
a closure that is used-then-dropped; this bug is that a *stored* capturing closure
is mis-dispatched and crashes before any lifetime question arises.
