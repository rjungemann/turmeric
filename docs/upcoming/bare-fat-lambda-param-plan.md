---
title: Bare `^fat` Parameter on Lambda Binders
category: Plan
status: Implemented
description: Allow a bare `^fat g` parameter binder in a `(fn [...] ...)` lambda, matching the surface already supported on top-level `defn` params. Previously the elaborator rejected calls of `g` inside the lambda body with "'g' is not a function", which blocked the lambda-retype path added for closure-returning instance methods from ever firing. Implemented: lambda binders now run the same `^fat` normalization as `defn` params.
---

# Bare `^fat` Parameter on Lambda Binders -- Plan

> **Status:** Implemented (2026-06-04). `elab_fn` now mirrors the `defn`-param
>   `^fat` path: a bare `^fat g` binder defaults to `:ptr<void>` + `is_fat`,
>   and an annotated `^fat g :(fn [...] :T)` binder pins the signature. The
>   `arg_fat` flags propagate into the lambda's `fn_type` for call-site
>   shimming. Fixtures: `tests/fixtures/{bare-fat-lambda-param,
>   annotated-fat-lambda-param,bare-fat-lambda-closure-returning}`.
> **Found:** 2026-06-03, while wiring the lambda retype step in
>   [closure-returning-instance-method-codegen-plan.md](closure-returning-instance-method-codegen-plan.md).
> **Severity:** Medium -- expressiveness hole. The retype is correct but
>   inert: any user-written lambda that would exercise it gets rejected at
>   elaboration time with "`g` is not a function", so the path is never
>   reached. No silent miscompile -- the call site is a hard error.
> **Related:**
> - [bare-fat-result-type-inference-plan.md](bare-fat-result-type-inference-plan.md)
> - [closure-returning-instance-method-codegen-plan.md](closure-returning-instance-method-codegen-plan.md)
> - [archive/fat-fn-param-capturing-closure-gap.md](../archive/fat-fn-param-capturing-closure-gap.md) (resolved for `defn`)

---

## Summary

A bare `^fat g` parameter on a top-level `defn` is now accepted: the binder
defaults to a `:ptr<void>` fat box and `(g x ...)` dispatches through the fat
protocol. The same surface on a lambda is not yet supported:

```turmeric
;; defn:  OK -- 'g' is callable, dispatched as fat
(defn run-with [^fat g x : int] : int
  (g x))

;; fn:    REJECTED -- elaborator: "'g' is not a function"
(let [run (fn [^fat g x : int] : int
            (g x))]
  (run (make-adder 10) 5))
```

The lambda-side gap blocks the lambda retype step in the closure-returning
instance method codegen plan from ever firing on user-written lambdas: the
retype is correct (the box carrier flips to `int64_t` and the captured
handle threads through), but the call site never elaborates, so the rewrite
has no input.

## Observed vs. expected

**Observed.** `(fn [^fat g ...] (g ...))` elaborates the parameter binder
without surfacing the `^fat` annotation into the per-arg type / callable
classification used by the elaborator's call check. When the body reaches
`(g ...)`, `g` is still tagged as an opaque value (or a raw `:ptr<void>`),
and the call-form check errors with **"'g' is not a function"**.

**Expected.** Same behavior as on a `defn` param:

- the binder gets a fat-box type (`:ptr<void>` by default, or the annotated
  `(fn [...] :T)` if one is supplied),
- the body can call `g` like any other callable,
- the call lowers through the existing fat-dispatch path
  (`__tur_fat_apply_*` / closure trampoline).

## Root-cause sketch

The `defn` parameter pipeline runs the `^fat`-binder normalization that:

1. records the param as `:ptr<void>` when no fn-type annotation is present,
2. flags the binder as fat-callable so the elaborator's call check treats
   `(g ...)` as a fat dispatch rather than a regular bare call,
3. participates in the bare-`^fat` result-type inference pass described in
   [bare-fat-result-type-inference-plan.md](bare-fat-result-type-inference-plan.md).

The `fn` (lambda) parameter pipeline does step (1) only partially and skips
(2) entirely, so even with `(bare-fat-result-type-inference-plan)` already
running on lambda bodies, the call check trips before inference ever runs.

The exact site is the lambda-binder elaboration path that mirrors the
`defn`-param path. A pointer to investigate: the per-binder normalizer
that consumes `^fat` -- whichever helper that is in the elaborator (likely
shared with `defn` after a small refactor) needs to be invoked on lambda
binders too, with the same downstream flag-setting.

## Proposed work

### Phase 1 -- accept `^fat` on lambda params

Lift the `^fat`-binder normalizer out of the `defn`-param-specific path and
invoke it for `fn`-param binders as well. After this, `(fn [^fat g ...]
(g ...))` elaborates and lowers exactly like the `defn` analogue, including
the fat-dispatch call site.

**Acceptance:** the fixture below compiles and prints `15`.

```turmeric
(defn make-adder [n : int] : ptr<void>
  (fn [x : int] : int (+ x n)))

(defn main [] : int
  (let [run (fn [^fat g x : int] : int
              (g x))]
    (println-int (run (make-adder 10) 5)))    ; => 15
  0)
```

### Phase 2 -- annotated `^fat g :(fn [...] :T)` on lambda params

Mirror the annotated form from `defn`: a typed fn annotation pins the
signature, suppresses the bare-`^fat` inference pass for that binder,
and feeds the typed-shim path. No new mechanism -- just route the lambda
binder through the same code.

**Acceptance:** the annotated variant of the fixture above also compiles
and prints `15`, and the annotation is honored (e.g., a wrong-arity call
inside the body is rejected with the same diagnostic as on `defn`).

### Phase 3 -- documentation + fixtures

- Add a fixture under `tests/fixtures/` exercising both bare and annotated
  `^fat` on lambda params, including the closure-returning case from the
  instance-method retype plan.
- Cross-link from
  [bare-fat-result-type-inference-plan.md](bare-fat-result-type-inference-plan.md)
  ("the pass now also fires on `fn` binders with `^fat`") and from
  [closure-returning-instance-method-codegen-plan.md](closure-returning-instance-method-codegen-plan.md)
  ("the lambda retype now has reachable input").

## Validation

- `bash tests/run.sh` is green with the new fixtures.
- The retype step from the closure-returning instance-method plan is
  exercised by at least one fixture whose lambda uses bare `^fat`.
- The error "'g' is not a function" no longer appears for the
  `(fn [^fat g ...] (g ...))` shape; a wrong use (e.g., calling a non-fat
  value as fat) still produces a clear, targeted diagnostic.

## Out of scope

- Curried / nested lambdas with `^fat` in inner positions beyond what
  already works on `defn` -- if `defn` does not yet handle it, defer to a
  follow-up rather than expanding scope here.
- Any change to the fat dispatch ABI or the typed-shim path; this plan is
  purely about surface acceptance and elaborator routing.
