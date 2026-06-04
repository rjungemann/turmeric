---
title: Applying the Result of a Higher-Order-Returning Parameter Is "Not Callable"
category: Reported Bug
description: A parameter whose declared type is a function that *returns a function* -- e.g. `f : (fn [int] (fn [int] int))` or the `->` spelling `f : (-> int (-> int int))` -- loses the inner function type at the application site. The first application `(f 1)` type-checks, but the *second* application `((f 1) 2)` fails with `TUR-E0002: function '__call_head_*' returns ?, which is not callable`. The result-of-application type is erased to "?" rather than recovered as the inner `(fn [int] int)`. Surfaced while implementing fn-type-bare-identifier-plan (the curried fixture case).
---

# Applying the Result of a Higher-Order-Returning Parameter Is "Not Callable" -- Reported Bug

> **Status:** OPEN (found 2026-06-04).
> **Found:** while implementing the curried fixture case of
>   [fn-type-bare-identifier-plan](../upcoming/fn-type-bare-identifier-plan.md).
> **Severity:** Medium -- a hard, non-silent error (`TUR-E0002`), so it
>   does not miscompile. But it blocks an entire class of curried
>   higher-order signatures from being *used* (the signature type-checks,
>   only the second application is rejected), forcing the `:ptr<void>` +
>   annotated-`^fat` recovery workaround that the typed-signal plans
>   already lean on.

---

## Summary

When a `defn` parameter is declared with a function type whose **result is
itself a function type**, the compiler accepts the signature and accepts the
*first* application, but rejects the *second* application: the type of
`(f x)` is erased to `?` instead of being recovered as the declared inner
function type.

This is **independent of the leading-colon syntax** -- it reproduces
identically with the bare-identifier form, the legacy `:`-keyword form, and
the `->` spelling.

## Repro

```turmeric
;; bare-identifier (fn ...) form
(defn curried [f : (fn [int] (fn [int] int))] : int
  ((f 1) 2))
(defn main [] : int 0)
```

```turmeric
;; legacy keyword (fn ...) form -- same failure
(defn curried [f : (fn [:int] (fn [:int] :int))] : int
  ((f 1) 2))
```

```turmeric
;; arrow form -- same failure
(defn curried [f : (-> int (-> int int))] : int
  ((f 1) 2))
```

All three produce:

```
error [TUR-E0002]: function '__call_head_*' returns ?, which is not
callable -- did you mean to pass all 0 argument(s)?
  ((f 1) 2)
  ^^^^^^^^^
```

### Observed vs. expected

- **Observed:** `(f 1)` is treated as returning `?` (unknown), so the outer
  application `((f 1) 2)` is rejected as "not callable".
- **Expected:** `(f 1)` has type `(fn [int] int)` (the declared result of
  `f`), so `((f 1) 2)` is a well-typed `int`.

The *signature alone* type-checks fine -- `(defn higher [f : (fn [int] (fn
[int] int))] : int 0)` passes `tur check`. Only the nested application is
rejected.

## Root cause (suspected)

The result type of an application is recovered from the callee's stored
function type. For a parameter typed `(fn [A] (fn [B] C))`, the *outer* fn's
result slot is a `TY_FN`, but the application-elaboration path appears to
collapse the result to its `TypeKind` (a pointer/closure carrier) and drop
the full `Type *` for the inner `(fn ...)`. The second application then has
no inner function type to dispatch on, so the callee type is "?".

Two relevant sites in `src/compiler/elab_types.c`:

- The `(fn ...)` type-expression handler (~`elab_types.c:901-957`) builds the
  type with `type_fn(fn_arg_kinds, n_fn_args, ret_kind)` and sets only
  `effect_row`. Unlike the `->` handler (~`elab_types.c:1063-1086`), it does
  **not** populate `arg_full_types` / `result_full_type` when an arg or the
  result is itself a compound (e.g. `TY_FN`) type. *However*, fixing only
  this does not resolve the bug: the `->` form -- which *does* store full
  types -- fails identically, so the erasure is (also) on the application /
  call-result-recovery side, not solely in the `fn` type-expression builder.

The call-result recovery lives in the call-elaboration path
(`src/compiler/elab_call.c`, the `__call_head_*` synthesis); it should read
the callee fn type's `result_full_type` (when present) and use it as the type
of the application node, so a chained application sees a `TY_FN` rather than a
bare result kind.

## Proposed fix directions

1. In the `(fn ...)` type-expression handler, mirror the `->` handler: when
   any param or the result is a compound type (`TY_FN`, `TY_APP`, `TY_FORALL`,
   ...), store `arg_full_types` / `result_full_type` on the synthesized
   `TY_FN` so the inner function type survives.
2. In the call-result-recovery path, when the callee's fn type carries a
   `result_full_type`, propagate it as the type of the application expression
   (instead of reconstructing a bare type from the result `TypeKind`). This is
   what lets the *second* application find a callable `TY_FN`.
3. Add a runnable regression fixture exercising
   `((f 1) 2)` through a curried parameter once both sides thread the inner
   type.

## Validation when fixed

1. All three repros above compile, and a runnable program where `f` is
   `(fn [a] (fn [b] (+ a b)))` applied as `((f 1) 2)` prints `3`.
2. `bash tests/run.sh` stays green (zero new FAILs).
3. The curried case in `tests/fixtures/fn-type-bare-identifier/` can be
   upgraded from signature-only to a real nested application.

## Cross-references

- Consumer-side mirror of
  [fn-typed-return-lowered-to-result-type](fn-typed-return-lowered-to-result-type.md)
  (producer-side return lowering, resolved). That fix threaded the inner type
  through the *producer* signature; this bug is the *application/consumer*
  side of the same "inner fn type erased" family.
- Surfaced by
  [fn-type-bare-identifier-plan](../upcoming/fn-type-bare-identifier-plan.md)
  (Phase 1 curried fixture case).
