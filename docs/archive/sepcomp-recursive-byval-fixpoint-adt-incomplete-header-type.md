# Separate compilation: recursive by-value fixed-point ADT emits an incomplete-type field in the header

> **RESOLVED (2026-07-22).** Root cause was header type-emission ORDER, not the
> layout itself: `emit_header` (`src/compiler/emit_module.c`) flushed the
> monomorph type-applications (`type_codegen_emit_adt_apps`) BEFORE the base
> `tur_adt_<Name>` typedefs, so `tur_adt_GNodeF__GNode` -- which embeds base
> `tur_adt_GNode` by value -- named an incomplete forward decl. Fixed by
> mirroring `emit_program`'s Pass-0 ordering: base ADT typedefs that do NOT
> themselves embed a monomorph by value (`adt_has_inline_byval_monomorph_field`
> is false -- e.g. glsl's `Roll` newtype `GNode`, which holds an int64 carrier)
> are now emitted in a "Pass A" BEFORE the monomorph flush; bases that DO embed
> a monomorph by value stay in "Pass B" after it (the `TUR_TD_<Name>` guard
> makes the second pass a no-op for Pass-A types). The monomorph now sees a
> complete base type.
>
> **Verification:** `glsl`, `c-dsl`, `scscm`, `template` now build clean as
> `--shared` libraries; `ansi`/`ecs`/`signal`/`linalg`/... unaffected (17/21
> pure spices build, the 4 remaining being external-dep/spice-source). glsl's
> test suites pass under ASan (ir 21, shaders 43, codegen 46 -- exercising the
> `GNode` cata/fold). Full turmeric suite: 2267 passed, 2 failed (both
> pre-existing: `re-string`, `vec-push-byvalue-aggregate`).

**Severity:** medium (blocks `tur build --shared` / project-mode builds of any
spice whose public API rests on a `:copy` fixed-point ADT; whole-program
`emit-c` of the same code is unaffected). Pre-existing; surfaced while auditing
`../turmeric-spices` against the recent compile/link (separate-compilation)
work.

## Symptom

Building a multi-module spice that defines a by-value fixed point of a functor
(`GNode ~= GNodeF GNode`) as a shared library fails at C compile:

```
glsl__ir.h:201:49: error: field has incomplete type 'tur_adt_GNode'
```

Reproduced by `spices/glsl`, `spices/c-dsl`, `spices/scscm`, `spices/template`
(all define recursive tree/IR node ADTs).

## Minimal repro

```turmeric
;; ir.tur
(defdata GNodeF :copy [a])                 ;; functor with recursive slots typed `a`
;; ... constructors GBinF/GSwizzleF/... each holding `a` positions ...
(defdata GNode  :copy (Roll (GNodeF GNode)))   ;; by-value fixed point
(defn unroll-node [n : GNode] : (GNodeF GNode) (match n (Roll l) l))
```

`tur build --shared <dir>` (or any project-mode build that emits a per-module
header) fails; `tur emit-c` of a single combined file does not.

## Root cause

The separate-compilation **header** emits the monomorphized type-application
`tur_adt_GNodeF__GNode` (via `type_codegen_emit_adt_apps`, guard prefix
`TUR_TY_`) with its recursive slots laid out **by value**:

```c
// glsl__ir.h (generated)
typedef struct tur_adt_GNode tur_adt_GNode;         // forward decl only (incomplete)
...
typedef struct tur_adt_GNodeF__GNode {
    int tag;
    union {
        struct { const char * _0; tur_adt_GNode _1; tur_adt_GNode _2; } GBinF;  // <-- by-value field of an incomplete type
        ...
    };
} tur_adt_GNodeF__GNode;
```

`tur_adt_GNode` is only ever forward-declared in the header (`typedef struct
tur_adt_GNode tur_adt_GNode;`), never completed, because the `Roll` newtype's
by-value payload *is* `tur_adt_GNodeF__GNode`, and vice-versa: the two structs
embed each other by value, which is not representable in C (infinite size). The
whole-program path (`emit_program`, `src/compiler/emit_module.c`) evidently
lowers at least one of the two recursive positions through the int64/pointer
carrier so the cycle is broken; the split-header monomorph emitter
(`type_codegen_emit_adt_apps`, reached from `emit_header` at
`src/compiler/emit_module.c:11344`) lays the slot out by value and produces the
incomplete-type field.

## Fix directions

- The recursive slot of a `:copy` fixed-point ADT must cross as a pointer/carrier
  (the `Roll`/`unroll` boundary is the natural indirection point), not by value,
  in the header's monomorph layout -- match whatever representation the
  whole-program path already picks so header and `.c` agree. Look at where
  `type_codegen_emit_adt_apps` chooses the field C type for a slot whose type is
  (transitively) the ADT being defined, and force the carrier there.
- Alternatively, detect the by-value self/mutual recursion at elaboration and
  reject `:copy` on a fixed point (require the boxed representation), so the
  incompatible layout is never requested.

## Related

- Base non-recursive by-value defstruct/ADT typedefs missing from the
  separate-compilation header were fixed in this same audit (emit_header now
  emits the guarded `TUR_TD_<Name>` base layout for module-local ADTs); that fix
  cleared `ansi`/`ecs`/`signal` but does not address the recursive by-value
  layout above.
- [[project_monomorphization_north_star]] -- the by-value/carrier representation
  split is the same hybrid-ABI seam.
