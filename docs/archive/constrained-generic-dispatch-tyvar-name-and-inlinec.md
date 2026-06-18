# Constrained-generic dispatch: tyvar-name dependence + inline-C receiver ABI

**Status:** RESOLVED (fixed in the same session that filed it).

Two related defects blocked a `^Backend`-generic helper in the plot spice,
building against turmeric main @ 8a87162 (PR #439). The concrete typeclass
collapse worked; the generic form did not. Both are fixed here, with two new
regression fixtures.

## Bug 1 -- dispatch resolution depended on the constraint tyvar's *name*

**Severity:** silent expressiveness hole / spurious hard error. A
constrained-generic helper only compiled for certain tyvar spellings; the same
body with only the tyvar renamed failed type checking.

### Repro

```turmeric
(defclass Backend [b]
  (render-to [self : b  x : int] : int))
(defstruct CanvasBackend [canvas : int  px : int  py : int  pw : int  ph : int])
(defstruct PngBackend   [path : cstr  tag : int])
(definstance Backend [CanvasBackend] (render-to [self x] (+ (.canvas self) x)))
(definstance Backend [PngBackend]    (render-to [self x] (+ (.tag self) x)))

(defn draw [^Backend B b : B x : int] : int          ; <- only the tyvar `B` changes
  (render-to b x))

(defn main [] : int
  (println (draw (make-struct CanvasBackend 100 0 0 7 0) 1))
  (println (draw (make-struct PngBackend "x" 50) 3))
  0)
```

`sed`-ing only the tyvar:

| tyvar spelling | result (pre-fix) |
| --- | --- |
| `A B C D E F G H K V` | compiles, prints `108` / `53` |
| `I J L M N O P Q R S T U W X Y Z`, any multi-letter (`Bk`, `TT`, `El`, `Zz`) | `[TUR-E0001] function 'draw' arg 1: expected <struct>, got CanvasBackend` |

### Root cause

In `elab_fns.c`, the inline `^Class Binder` parser explicitly **discarded** the
binder name -- "For v1, we ignore the type variable name and just record
constraints." So `B` was never added to the function's `fn_type_params`.

When the later parameter `b : B` was resolved, `B` was not a known type
parameter, so it fell through to the **KB-026 demotion pass**
(`elab_fns.c`, ~line 2236): a parameter typed by a name that is not declared in
the type-param list, not a kind variable, and does not occur in >=2 type
positions is rewritten to the anonymous `<struct>` placeholder
(`TY_STRUCT{def=NULL}`). `B` occurs only once here (the `b` param), so it was
demoted -- and the call-site arg check then "expected `<struct>`" and rejected
`CanvasBackend`.

The *only* reason some spellings escaped demotion is the
`fn_name_is_adt_tyvar(e, nm)` escape hatch in that same pass: a name that
happens to match **some loaded ADT/struct's declared type-param name** is
treated as a genuine type variable. The builtin/prelude ADTs happen to use
`A`/`B`/.../`K`/`V` as type-param names, so dispatch "worked" for those
spellings by pure coincidence -- a textbook "works by luck because the names
happen to match" trap. `T`/`X`/`Z` and every multi-letter binder collide with
nothing, so they were demoted.

### Fix

`src/compiler/elab_fns.c`: when the `^Class Binder` parser consumes the binder
symbol, **register it as a function type parameter** (`fn_type_params` +
`KIND_STAR`) and record it as the constraint's `.tyvar` (mirroring the `where
(Class tv)` clause path, which already did this). `b : B` then resolves to a
genuine named `TY_TYVAR` for every spelling, KB-026 no longer demotes it, and
dispatch no longer depends on the tyvar's name.

## Bug 2 -- the generic helper miscompiled inline-C concrete call sites

**Severity:** hard cc error; adding a generic helper broke previously-green
direct call sites in the same module.

### Repro

The plot spice's `Backend` instances render via **inline C**. An inline-C
instance method declares its struct receiver **by value** even above the
16-byte pass-by-pointer threshold (`emit_fns.c` §643/737 keys this off
`fd->body->kind == EX_INLINE_C`). With a generic helper present:

```turmeric
(definstance Backend [CanvasBackend]            ; CanvasBackend = 5 ints = 40 bytes
  (render-to [self x] ```c return self.canvas + self.pw + x; ```))

(defn draw-canvas-direct [c : int w : int x : int] : int   ; direct call site
  (render-to (make-struct CanvasBackend c 0 0 w 0) x))

(defn draw [^Backend B b : B x : int] : int                ; generic helper
  (render-to b x))
```

```
error: incompatible type for argument 1 of '__inst_Backend_render_hyto_CanvasBackend'
   return __inst_Backend_render_hyto_CanvasBackend(&__t42, x);
   note: expected 'CanvasBackend' but argument is of type 'CanvasBackend *'
```

### Root cause

Instance-method `FnDef`s are built in `elab_definstance` (`elab_typeclasses.c`),
which set `method_fd->body` but **never stamped `method_fd->binding->
body_is_inline_c`**. So that flag was always false on every instance method.

Both call-site ABI paths key off that flag to decide whether to take the
address of a pass-by-ptr struct argument:

- the Phase D `&temp` pass-by-ptr spill in `emit_expr.c` (direct call sites),
  guarded by `!fn_binding->body_is_inline_c`; and
- #439's `emit_reresolved_receiver_is_by_ptr` bridge in `emit_core.c` (generic
  monomorphizations), guarded by `impl->binding->body_is_inline_c`.

With the flag never set, both wrongly took `&receiver` and passed a
`CanvasBackend *` to the inline-C instance method's by-value `CanvasBackend
self` formal -- re-triggering exactly the pass-by-pointer-threshold ABI mismatch
#439 set out to fix, the moment an inline-C instance crossed the threshold.

### Fix

- `src/compiler/elab_typeclasses.c`: stamp `method_fd->binding->
  body_is_inline_c` from the elaborated body, the same way `elab_fns.c` does for
  ordinary defns. This is the common root cause -- it fixes both the direct
  call site and the generic monomorphization at once.
- `src/compiler/emit_core.c`: make `emit_reresolved_receiver_is_by_ptr`
  self-contained by also checking `impl->body->kind == EX_INLINE_C` directly
  (mirroring `emit_fns.c` §643), so the predicate is robust even if the cached
  binding flag is ever absent.

## Validation

- Two new fixtures:
  - `tests/fixtures/constrained-generic-dispatch-tyvar-name-independence`
    (Bug 1; uses the `Zz` binder, which collides with no ADT type-param, so a
    pre-fix compiler fails it).
  - `tests/fixtures/constrained-generic-inline-c-receiver-dispatch`
    (Bug 2; inline-C instances exercised at both a direct call site and through
    the generic helper, with a 40-byte pass-by-ptr receiver).
- `bash tests/run.sh`: `1686 passed, 0 failed`, no codegen-snapshot churn.
- The pre-existing `constrained-generic-struct-receiver-dispatch` fixture (the
  non-inline-C struct-receiver case #439 added) still passes unchanged.
