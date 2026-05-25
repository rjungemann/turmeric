# Plan: Unblock the signal spice typecheck skip

> **Status:** All issues resolved -- signal spice fully unblocked
> **Last Updated:** 2026-05-25
> **Type:** Compiler bug fix / Spice maintenance
> **Companion to:** [skipped-spices-cleanup-plan.md](skipped-spices-cleanup-plan.md)
> **Tracked across repos:** `turmeric` (compiler fixes), `turmeric-spices` (spice source fixes)

---

## Overview

After the 8 originally typecheck-skipped spices were unblocked, the
signal spice remains the only one with a `requires.typecheck-skip` marker.
It was added because `tur check` fails on
`turmeric-spices/spices/signal/src/signal/synth.tur` with two distinct
root causes.

**Goal:** fix both root causes and delete
`../turmeric-spices/spices/signal/requires.typecheck-skip` so the SC8
matrix covers the signal spice.

---

## Background: what already passes

Running `tur check` against the signal spice files individually shows:

| File | Status |
|------|--------|
| `src/signal/core.tur` | PASS |
| `src/signal/dsp.tur` | PASS |
| `src/signal/envelope.tur` | PASS |
| `src/signal/synth.tur` | FAIL (two issues below) |

---

## Issue 1: Higher-order / let-bound function calls

### Symptom

Calling a let-bound variable that holds the return value of a function that
itself returns a function type (`TY_FN`) triggers a compiler crash:

```
runtime error: member access within misaligned address 0x000032aaaba2 for type
'const EffectRow', which requires 8 byte alignment
```

### Minimal reproducer

```turmeric
(defn make-sf []
  (fn [sig]
    (fn [t]
      (+ sig t))))

(defn test []
  (let [sf (make-sf)]
    (sf 2)))
```

### Root cause (Issue 1a -- crash, **now fixed**)

There are two definitions of `type_from_kind` in the codebase:

1. `src/compiler/types.c` -- file-`static`, uses `memset(&t, 0, sizeof(t))`
2. `src/compiler/elab_core.c` -- the one actually called from elaboration code
   (declared in `elab_internal.h`), which did **not** use `memset`

The `elab_core.c` version manually initialized only a handful of fields
(`kind`, `copy_kind`, `as.fn.arity`, `n_lifetimes`, `typeclass_instances`,
`n_typeclass_instances`, `hkt_kind`), leaving all other fields -- including
`as.fn.effect_row` -- with whatever garbage happened to be on the stack.

When a `defn` returns a static anonymous `fn` (zero captures), the compiler
infers `result_kind = TY_FN` for that `defn`.  At the call site
`(make-sf)`, `elab_call_fn` computes the result type as
`type_from_kind(TY_FN)`, whose `as.fn.effect_row` is a garbage pointer
(`0x000032aaaba2` in observed crashes).  The let-binding for `sf` stores
this garbage type.  When `(sf 2)` is then elaborated, `elab_call_fn`
reads `fn_type.as.fn.effect_row` and passes it to
`effect_row_contains_symbol` (`elab_core.c:1399`), which dereferences the
misaligned pointer, causing a UBSan SEGV.

**Fix applied** (`src/compiler/elab_core.c`):

```c
/* Before */
Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.as.fn.arity = 0;
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.hkt_kind = KIND_STAR;
    return t;
}

/* After */
Type type_from_kind(TypeKind k) {
    Type t;
    memset(&t, 0, sizeof(t));          /* zero ALL fields, including effect_row */
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.hkt_kind = KIND_STAR;
    return t;
}
```

With this fix the crash no longer occurs.  The compiler now emits a proper
type error instead.

### Root cause (Issue 1b -- type info loss, **fixed**)

Even after fixing the crash, `(sf 2)` produces:

```
error [TUR-E0002]: function 'sf' returns ?, which is not callable
  -- did you mean to pass all 0 argument(s)?
```

The underlying problem is that `type_from_kind(TY_FN)` returns a zeroed
`TY_FN` type (arity=0, result_kind=TY_NIL), discarding the actual function
signature (arity=1, result_kind=TY_PTR_VOID) of the value that `make-sf`
returns at runtime.

The session type machinery already solves an analogous problem via
`result_full_type`: when a function's return type is `TY_SESSION`, the
compiler stores a pointer to the full session type and retrieves it at the
call site (`elab_call.c` near line 1460).  The same pattern needs to be
applied for `TY_FN` return types.

**Fix applied** (`src/compiler/elab_fns.c`, `src/compiler/elab_call.c`):

1. In `elab_fns.c` (`elab_defn` / `elab_fn`), when the body's type is
   `TY_FN`, a heap-allocated copy of the full inner `Type` is stored in
   `fn_type.as.fn.result_full_type` -- analogous to the `SS3a` path for
   `TY_SESSION`.

2. In `elab_call_fn` (`elab_call.c`, near the result-type computation),
   `result_full_type` is used when available:
   ```c
   if (fn_type.as.fn.result_full_type) {
       result_type = *fn_type.as.fn.result_full_type;
   } else {
       result_type = type_from_kind(result_kind);
   }
   ```

The minimal reproducer now passes `tur check` with exit 0.

---

## Issue 2: Float-as-int type mismatches in `synth.tur`

### Symptom

The signal spice represents all `Sample` values as `float64` stored in
`int64` bitfields.  Functions that manipulate samples use parameters
declared without explicit type annotations, so the typechecker defaults
them to `:int`.  When float literals (e.g. `0.01`, `0.8`) are passed at
call sites, the typechecker reports a type mismatch:

```
error: arg 1: expected :int, got :float
```

### Root cause

- Untyped parameters default to `:int` in Turmeric.
- Float literal syntax (`0.01`) produces a `:float` expression.
- The signal spice relies on bit-casting between `float64` and `int64`
  at the C level, but the Turmeric source uses float literals directly,
  making the typechecker see a mismatch.

### Proposed fixes

**Option A (minimal -- annotate call sites with `as`):**
Wrap each offending float literal in an explicit cast:
```turmeric
(make-some-sample (as :int 0.01))
```
This is ugly and not idiomatic.

### Fix applied (Option B)

All `Sample`-typed parameters in `synth.tur` were annotated with `:int`.
The bit-cast semantics are handled by the C runtime; the typechecker now
agrees on `:int` throughout.

---

## Work plan

| Step | Owner | Status | Description |
|------|-------|--------|-------------|
| 1a | compiler | **Done** | Add `memset` to `elab_core.c:type_from_kind` to fix crash |
| 1b | compiler | **Done** | Propagate `result_full_type` for `TY_FN` returns (analogous to SS3a) |
| 2  | spice | **Done** | Annotate `Sample` params in `synth.tur` with `:int` |
| 3  | spice | **Done** | Verify `tur check` passes on all `src/` files in the signal spice |
| 4  | spice | **Done** | Delete `../turmeric-spices/spices/signal/requires.typecheck-skip` |

---

## Validation

Once steps 1b and 2 are complete, run:

```sh
tur check ../turmeric-spices/spices/signal/src/signal/core.tur
tur check ../turmeric-spices/spices/signal/src/signal/dsp.tur
tur check ../turmeric-spices/spices/signal/src/signal/envelope.tur
tur check ../turmeric-spices/spices/signal/src/signal/synth.tur
```

All four must exit 0.  Then delete the skip marker and confirm CI passes:

```sh
rm ../turmeric-spices/spices/signal/requires.typecheck-skip
```
