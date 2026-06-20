# Constrained-generic dispatch miscompiles pass-by-pointer struct receivers

**Status:** RESOLVED (fixed in the same session; this report is the change
description).

**Severity:** Silent-miscompile-class defect surfacing as a hard `cc` error.
A constrained-generic helper over a typeclass whose instances are *structs*
emitted a `T` argument into a `const T *` formal. For struct receivers above
the pass-by-pointer threshold this is a hard compile error; single-field
structs "worked" only because they share the int64 carrier's by-value ABI --
the exact "works by luck because the register classes happen to match" trap
CLAUDE.md flags.

## Context

This was reported downstream as a "scoped limitation" of the `plot` spice
(turmeric-spices) `Backend` typeclass:

> Writing a function generic over an abstract Backend -- e.g.
> `(defn draw [b ...] (render-to b ...))` with `b` unconstrained -- is not a
> cheap add. [...] Enabling abstract-Backend-polymorphic helpers would require
> turmeric-side constraint/dictionary-passing support -- out of scope here.

The premise is wrong in one important way: turmeric **already** supports
constraint-driven generics via emit-time ABI specialization (the `^Class K`
syntax, the `cgi-constrained-generic-dispatch` / `defn-class-constraint-list-syntax`
fixtures). The Backend-polymorphic helper *is* writable today as

```turmeric
(defn draw [^Backend B b : B  renderers : int opts : int] : ptr<void>
  (render-to b renderers opts))
```

What was actually missing was correct codegen for that form when the typeclass
instances are **structs** (gap H item 3, "still open for struct-receivers").
The `plot` spice's `Backend` instances -- `CanvasBackend` (5 fields),
`SurfaceBackend` (1 field), `PngBackend` (cstr + int) -- are precisely this
shape, so two of the three would have miscompiled.

## Minimal repro

```turmeric
(defclass Backend [b]
  (render-to [self : b  x : int] : int))
(defstruct CanvasBackend [canvas : int  px : int  py : int  pw : int  ph : int])
(definstance Backend [CanvasBackend]
  (render-to [self x] (+ (+ (.canvas self) (.pw self)) x)))
(defn draw [^Backend B b : B x : int] : int (render-to b x))
(defn main [] : int
  (println (draw (make-struct CanvasBackend 100 0 0 7 0) 1)) ; want 108
  0)
```

### Observed (before fix)

```
error: incompatible type for argument 1 of '__inst_Backend_render_hyto_CanvasBackend'
   return __inst_Backend_render_hyto_CanvasBackend(b, x);
note: expected 'const CanvasBackend *' but argument is of type 'CanvasBackend'
tur: cc invocation failed
```

A single-field `CanvasBackend [canvas : int]` compiled and ran -- by luck,
because below the pass-by-ptr threshold both the spec clone's parameter and the
instance method's `self` are by value.

### Expected

`108` (and `202` / `1053` for the sibling `SurfaceBackend` / `PngBackend`
cases in the fixture).

## Root cause

`tur build`/`emit-c` realizes a constrained generic by cloning the body into
an ABI specialization (`draw__spec__int64_t_CanvasBackend_int64_t`). Inside the
clone, `emit_reresolve_method_call` (src/compiler/emit_core.c) correctly
re-targets the `render-to` call to `__inst_Backend_render_hyto_CanvasBackend`.

But the *name* is the only thing it rewrote. Argument emission
(src/compiler/emit_expr.c, EX_CALL) decides whether to pass a struct argument
by address from `fn_binding->type` -- which here is the **generic method**
whose receiver parameter is a bare `TY_TYVAR`. A tyvar param is not pass-by-ptr,
so `_callee_pbp` was `false` and no `&` was applied. Meanwhile the *resolved*
instance method takes `const CanvasBackend *self` (emit_fns.c emits a non-inline-C,
non-closure struct param above the threshold as `const T *`). Mismatch.

## Fix

1. `src/compiler/emit_core.c` -- new predicate
   `emit_reresolved_receiver_is_by_ptr(ctx, call)`. True only when, inside an
   ABI spec, a `dict_arg` method call's dispatch tyvar is the receiver (arg 0),
   it resolves to a struct/ADT that `type_struct_pass_by_ptr` reports as
   by-pointer, and the selected instance method actually emits its receiver by
   pointer (i.e. not an inline-C/closure body, which declare struct params by
   value). Returns false for the int-carrier base clone and scalar/single-field
   receivers, so no currently-resolving dispatch changes.

2. `src/compiler/emit_expr.c` -- when that predicate holds for arg 0, spill the
   by-value receiver to a temp and pass `&tmp`, mirroring the existing Phase D
   pass-by-pointer materialization.

3. Declaration added to `src/compiler/emit_internal.h`.

## Validation

- `tests/fixtures/constrained-generic-struct-receiver-dispatch/` -- new
  fixture with three deliberately different struct layouts plus a backend that
  dispatches *through* another (mirroring `PngBackend` -> `SurfaceBackend` in
  the plot spice). Expected `108 / 202 / 1053`.
- Full suite: `bash tests/run.sh` -- no new failures, no codegen-snapshot churn.
  (The single pre-existing `errors/ecs-defsystem-writes-unauthorized` failure is
  an unrelated turmeric-fixture-vs-spices-main diagnostic-sync issue in the ecs
  spice's `world.tur`; it reproduces without this change and is an
  elaboration-time error the codegen path never reaches. Tracked separately in
  docs/reported/.)
