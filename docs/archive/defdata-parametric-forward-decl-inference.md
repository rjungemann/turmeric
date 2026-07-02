---
title: Sibling forward-referenced defn's compound return type isn't visible to callers
severity: LOW-MEDIUM. Forces per-arm `(::)` ascription in mutually recursive
functions whose return type is a parametric ADT.
status: RESOLVED 2026-07-02. Pass-1 forward-decl scan now records compound
parametric-ADT return types as a full TY_APP.
---

# Forward-declared compound return type isn't visible to sibling callers

## Resolution (2026-07-02)

Fixed in `src/compiler/elab_toplevel.c`.  The Pass-1 defn forward-declaration
scan now handles a compound `(F A B)` return whose head symbol resolves to an
ADT/struct stub registered by RF0 (e.g. `(PRes Expr)`, `(Option Foo)`,
`(Vec T)`).  A new shallow, kind-check-free walker (`fwd_shallow_result_app` /
`fwd_shallow_type_arg`) builds the `TY_APP` chain by hand and stamps it onto the
forward decl's `result_full_type`; a sibling caller declared *earlier* in the
module then reads the concrete type args off the scrutinee, so the pattern
bindings no longer fall back to the placeholder carrier.

`fn_type_from_form` is deliberately **not** used for this: at Pass-1 time the
ADT stubs are not yet kinded, so it would emit a spurious `TUR-E0012` kind
mismatch (`cannot apply a type of kind '*' as a type constructor`) for every
parametric return in the stdlib.  The shallow walker skips kind checking
entirely, exactly as the report's "shallow walker that only reads pre-registered
names" direction anticipated.

The exact reduced repro below now `emit-c`s, builds, and runs with no per-arm
`(::)` ascription.  The regression fixture `parametric-fwd-decl-sibling-return`
guards it.

## Symptom

Given two mutually recursive functions where the callee (declared later) has
a parametric-ADT return type:

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))

(defgadt Expr [a] (ENum int : (Expr int)))

(defn factor [xs : int] : (PRes Expr)
  (match (expr-parse xs)                 ;; forward-reference
    (PFail)          (PFail)
    (POK inner rest) (POK inner rest)))  ;; error: (PRes int) vs (PRes Expr)

(defn expr-parse [xs : int] : (PRes Expr)
  (:: (PFail) (PRes Expr)))
```

The elaborator reports:

```
error: if branches have mismatched types:
  then=(type-app PRes int) else=(type-app PRes Expr)
```

`inner` should bind to `Expr` (from the scrutinee's declared `(PRes Expr)`),
but the pattern binding sees a scrutinee whose type args weren't recorded and
falls back to the placeholder `int` carrier for the tyvar field.  The
subsequent `(POK inner ...)` then infers `(PRes int)`, mismatching sibling
arms that produced `(PRes Expr)`.

## Root cause

`elab_toplevel.c`'s Pass-1 defn forward-declaration scan
(`fwd_decl_scan_params` + the return-type peek at ~line 1207) commits
only **primitive scalar** return kinds (`:int`, `:bool`, `:cstr`, ...) plus
one special-case for `Session[P]`.  A compound `(F A B)` return -- including
every parametric-ADT / GADT application like `(PRes Expr)`, `(Option Foo)`,
`(Vec T)` -- leaves the pre-registered forward decl with
`result_kind = TY_INT` and `result_full_type = NULL`.

The HRT5 late-update in `elab_fns.c` (~2360) *does* patch
`result_full_type` on the callee's own binding, but only when the callee's
body-elaboration starts -- too late for a caller declared earlier in the
same module.

## Fix directions

Extend the Pass-1 return-type peek in `elab_toplevel.c` to also handle
`F_TYPE_ANN` return forms whose head symbol resolves to a registered ADT
or struct stub (RF0 has already run at this point).  Build the `TY_APP`
chain from the annotation's arg forms -- either concrete types or
non-registered names that stay as `TY_UNKNOWN` fillers -- and stamp
`result_full_type` onto the forward decl.

`type_expr_from_form` is what already parses these annotations for the
real defn pass; the question is whether it's safe to invoke against a
scope that only has RF0 stubs (no registered defns yet).  A shallow
walker that only reads pre-registered names should be sufficient.

## Impact

Every parser-combinator style codebase (parsec-tutorial and any user
port) that uses mutually recursive functions over a parametric result
type hits this.  The workaround is a per-arm `(:: expr (PRes T))`
ascription on match arms whose peer arms produce concrete-parametric
values.

## Related

- [defdata-parametric-inference-and-elab-match-segv.md](defdata-parametric-inference-and-elab-match-segv.md)
  -- the direct-inference gap for bare `(PFail)` (fixed 2026-07-01);
  this issue is the *other* half of the tutorial's ascription burden.
