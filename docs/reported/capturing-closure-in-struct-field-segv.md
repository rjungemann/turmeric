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

## Implementation plan (verified against the tree, 2026-07-20)

The fix is a coordinated representation change, NOT a local patch. All the
machinery already exists -- a **TY_TYVAR (parametric) fn-field already does the
right thing** and is the template.

**The existing template (elab_call.c:2535-2549).** A thin fn stored into a
parametric ADT field (`ft->kind == TY_TYVAR`) is already auto-shimmed to a fat
box via `EX_FN_TO_FAT` + `boxed=true`, and its match-arm extraction is marked
`is_fat`, so both store and read agree on the fat `{thunk, env}` protocol. The
comment there even names this exact SEGV. The gap: a **concrete** `(fn ...)` field
(`H.unary`, `Adder.f`) is left on the THIN representation instead.

**Three coordinated parts:**

1. **Boxed field type (read side).** Mark a concrete fn-typed field's `full_type`
   `boxed = true` (`elab_structs.c` `resolve_ctor_field`, and preserve it through
   `struct_field_instantiate_type`'s `TY_FN` arm at `elab_structs.c:416`). A `(.f
   v)` read then yields a `boxed` TY_FN, so the call site takes the fat path
   (`TUR_APPLY*_T`) instead of the thin `((R(*)(A))v)(a)` cast -- the dispatch
   already keys on `type.as.fn.boxed` (`emit_expr.c:1153/1470/3977/4142`).

2. **Shim thin-fn stores (store side).** Extend the `EX_FN_TO_FAT` auto-shim loop
   (elab_call.c:2535) to fire for a boxed concrete fn-field, not only
   `TY_TYVAR` -- so a bare fn (`inc`) stored into the field becomes a fat handle.
   A capturing closure is already fat (TY_PTR_VOID env). Without this, a thin fn
   in a now-fat-dispatched field crashes symmetrically.

   Parts 1+2 are ATOMIC: either alone breaks the ~19 concrete-fn-field fixtures
   (`conv-defstruct-typed-fn-field-lowering`, `dot-parametric-fn-field-call`,
   the `instance-closure-return-*` set, `poly-to-fat-*`, ...). Their `expected.c`
   snapshots all change (thin cast -> `TUR_APPLY*`); regenerate in the same PR.

3. **Static fat box for bare fns (avoid a NEW leak).** The `EX_FN_TO_FAT` emitter
   (`emit_expr.c:7732`) `malloc`s the 2-slot `{shim, orig_fn}` box. Applied to
   parts 1+2, EVERY thin-fn-field store now heap-allocates a box the struct never
   frees -- trading the SEGV for a per-store leak across those 19 fixtures (the
   harness compiles without ASan, so it stays green but leaks). For a
   compile-time-known bare fn the box is constant, so emit a file-scope
   `static const int64_t __fatbox_<fn>[2] = {(int64_t)__tur_fatshim<N>,
   (int64_t)<fn>}` and store its address -- no malloc, no leak. (A capturing
   closure's heap env is the separate S2 drop-glue concern.)

**Then, and only then, S2** can attach `drop_glue_env_N` to the holding struct's
drop glue to free a stored *capturing* closure's env (Model U: storing moves the
closure so exactly one owner frees).

**Why it was not landed in the same pass as the report:** parts 1+2 are a
representation flip for a working, widely-used path (19 fixtures, snapshot churn)
whose store/read/coercion must stay consistent, and part 3 (static shims) is
required to avoid a regression -- a deliberate multi-part change, not a slice to
rush alongside the S1 leak work.

## Note

This is orthogonal to the fat-closure ENV LEAK work already landed (S1: freeing a
fresh/inline closure consumed by a non-retaining callee). That path fixed *freeing*
a closure that is used-then-dropped; this bug is that a *stored* capturing closure
is mis-dispatched and crashes before any lifetime question arises.
