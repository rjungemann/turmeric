# Constrained-generic carrier base miscompiles a struct-receiver class with mixed C ABIs

**Status:** RESOLVED (fixed in the same session; this report is the change
description). Branch `claude/backend-generic-mixed-abi-uoqfsf`, building on
`main` @ 2f29dbf (#440).

**Severity:** Hard `cc` error. A `^Backend`-generic helper over a typeclass
whose instances are structs miscompiled the generic's *own carrier base* the
moment the class mixed by-pointer and by-value receiver instances. The base
collapsed to a single hardcoded instance and passed the erased `int64_t`
receiver into that instance's by-value struct formal -- `incompatible type for
argument 1`. It broke compilation of every module that compiles the defining
module, even ones that never call the helper: **merely defining it was enough.**

## Context

This is the next layer under #439 (constrained-generic dispatch into a
pass-by-pointer `const T *self` formal) and #440 (tyvar-name dependence +
inline-C receiver ABI). Both of those fixed the *monomorphized* (called) path:
a per-callsite specialization clone dispatching to the concrete instance. They
did not touch the generic function's standalone **carrier base** -- the
non-specialized `int64_t`-ABI definition emitted for the binding itself.

The plot spice shape that surfaced it:

```turmeric
(defclass Backend [b]
  (render-to [self : b  renderers : int  opts : int] : ptr<void>))

(defstruct CanvasBackend  [canvas : int  px : int  py : int  pw : int  ph : int]) ; 5 fields -> const CanvasBackend *self (by-ptr)
(defstruct SurfaceBackend [width : int  height : int])                            ; 2 fields -> SurfaceBackend self (by-value)
(defstruct PngBackend     [path : cstr])                                          ; 1 field  -> PngBackend self    (by-value)

(definstance Backend [CanvasBackend]  (render-to [self r o] ...))
(definstance Backend [SurfaceBackend] (render-to [self r o] ...))
(definstance Backend [PngBackend]     (render-to [self r o] ...))

(defn render [^Backend B b : B  renderers : int  opts : int] : ptr<void>
  (render-to b renderers opts))
```

The generated carrier base:

```c
static void * render(int64_t b, int64_t renderers, int64_t opts) {
        return __inst_Backend_render_hyto_PngBackend(b, renderers, opts);
        /*                                           ^ int64_t into `PngBackend self` */
}
```

i.e. it (a) collapsed to a single hardcoded instance (`PngBackend`, the
representative the elaborator bakes for an abstract-tyvar receiver -- the
last-declared instance) instead of dispatching, and (b) passed the erased
`int64_t b` straight into a by-value `PngBackend` formal.

The concrete call sites were already correct (#439/#440): the monomorphized
`render__spec__..._SurfaceBackend(...)` / `..._PngBackend(...)` clones construct
the struct and pass it with the right ABI. **Only the generic's carrier base
was wrong**, and it reproduced whether or not `render` was exported and whether
or not anything called it.

It did *not* reproduce when all instances shared one ABI (all single-field
by-value, or all multi-field by-pointer): a uniform-ABI three-instance generic
emitted no broken base. The trigger is the **mix** of by-pointer and by-value
receiver instances under one class used as a constrained-generic receiver.

## Root cause

`src/compiler/emit_module.c`. The carrier base for a top-level generic is
suppressed by `emit_abi_fn_skip_generic` when it would be dead/ill-typed, but
two guards both miss the bare-tyvar-receiver shape:

1. **`emit_abi_fn_is_generic_unsafe`** (the general suppressor) only fires when
   a named tyvar is *nested inside a compound* arg/result type -- it explicitly
   excludes a **bare** `TY_TYVAR` arg (`arg->kind != TY_TYVAR && ...`). The
   receiver here is the bare class tyvar `b : B`, so the helper is never
   classified generic-unsafe and its carrier base is always emitted.

2. **The Gap H spec-scan** (the struct-receiver suppressor added with #439's
   family) only fires when a struct-arg *specialization was actually minted* --
   i.e. the generic was called with a struct receiver. When the generic is
   never called (or never called concretely), no spec exists, so the scan finds
   nothing and the carrier base is emitted anyway.

So a constrained generic with a bare-tyvar receiver over a struct-receiver
class falls through both guards. Its carrier base bakes the representative
instance (a concrete struct), and emitting `__inst_..._<Struct>(int64_t b, ...)`
into that struct's by-value/by-ptr formal is a hard `cc` error. (Uniform
single-field-by-value classes only "worked" by sharing the int64 carrier's
by-value ABI -- the same register-class-coincidence trap #439 called out.)

A carrier base over a type-erased `int64_t` receiver can never dynamically
dispatch correctly regardless -- every concrete use monomorphizes to a
per-instance clone -- so when it would bake a concrete struct receiver it is
dead code.

## Fix

`src/compiler/emit_module.c`:

- New `expr_dispatches_tyvar_to_struct_receiver(const Expr *body)` walks the
  defn body for a baked typeclass-method call (`call->dict_arg` is an `EX_DICT`
  with an instance and a method name) whose receiver arg 0 is a bare
  `TY_TYVAR` and whose baked instance takes that receiver as a non-carrier
  struct/ADT (`(TY_STRUCT || TY_ADT) && !type_uses_carrier_abi`). This mirrors
  `emit_reresolve_method_call`'s receiver detection and identifies exactly the
  carrier base that would emit `int64_t -> T`.

- `emit_abi_fn_skip_generic` gains a fallback after the Gap H spec-scan: for a
  constrained generic (`constraints.n_constraints > 0`) whose body matches the
  predicate, skip the carrier base under the same `!emit_abi_has_carrier_call`
  guard the rest of the function uses. If a genuine carrier (polymorphic relay)
  call to the binding was observed, the base is still emitted -- behavior is
  strictly unchanged there, so this is a pure dead-code removal, never a
  regression of a currently-resolving dispatch.

The called/monomorphized path is untouched -- it was already correct after
#439/#440.

## Validation

- New regression fixture
  `tests/fixtures/constrained-generic-mixed-abi-uncalled-carrier/`: a
  `^Backend`-generic `render` over `CanvasBackend` (5 fields, by-ptr) +
  `SurfaceBackend` (2 fields) + `PngBackend` (1 field), both by-value -- the
  mixed-ABI shape. `render` is **defined but never called**; output comes from
  direct concrete `render-to` dispatch, so the generic's carrier base is the
  only thing the fix exercises. Pre-fix this failed to compile with the exact
  `incompatible type for argument 1 of '__inst_Backend_render_hyto_PngBackend'`
  error; post-fix it builds and prints `106 / 202 / 13`.

- The #439 fixture `constrained-generic-struct-receiver-dispatch` already
  covers the *called* mixed-ABI path (its `CanvasBackend` spec passes the
  receiver by `&tmp` / by-pointer while `SurfaceBackend` / `PngBackend` pass by
  value), so no second called-path fixture is needed.

- Full suite: `1686 passed, 0 failed`, no codegen-snapshot churn.
