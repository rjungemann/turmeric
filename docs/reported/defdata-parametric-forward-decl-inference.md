---
title: Sibling forward-referenced defn's compound return type isn't visible to callers
severity: LOW-MEDIUM. Forces per-arm `(::)` ascription in mutually recursive
functions whose return type is a parametric ADT.
status: OPEN. Found 2026-07-01 while cleaning up the parsec-tutorial fixture.
---

# Forward-declared compound return type isn't visible to sibling callers

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
