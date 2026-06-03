---
title: Intra-Instance Method Dispatch (`.method self`) Is Unsupported
category: Reported Bug
description: Calling one typeclass method from inside another method's body of the same instance -- via `(.other self ...)` -- fails elaboration with "no typeclass method found for '<other>'". Dispatch on the receiver works fine from outside the instance; only the in-instance-body case fails.
---

# Intra-Instance Method Dispatch (`.method self`) Is Unsupported -- Reported Bug

> **Status:** OPEN (found 2026-06-03)
> **Severity:** Medium -- an **ergonomics / expressiveness gap** that surfaces
>   as a hard elaboration error, not a miscompile. It blocks the natural
>   "method B is defined in terms of method A" idiom, forcing a shared
>   top-level helper instead.
> **Found:** while executing
>   [closure-returning-instance-method-codegen-plan](../upcoming/closure-returning-instance-method-codegen-plan.md);
>   surfaced designing the T6.5 "via other method" coverage fixture.

## Summary

Inside a `definstance` method body, dispatching to another method of the **same
instance** on the receiver `self` -- `(.base self ...)` -- fails to resolve:

```
error: no typeclass method found for 'base'
```

The receiver is correctly typed as the instance type, and the exact same
dispatch works from any *other* function (e.g. from `main`). The failure is
specific to a `.method` call that appears while the enclosing `definstance` is
still being elaborated.

## Minimal repro

```turmeric
(defclass HasN [a]
  (base  [self n :int] :int)
  (twice [self n :int] :int))
(defstruct ArrW [a] (raw :int))
(definstance HasN [ArrW]
  (base  [self n :int] (+ n 1))
  (twice [self n :int] (.base self (* n 2))))   ;; <- error here
(defn main [] :int
  (let [w (:: (make-struct ArrW 0) (ArrW int))]
    (println (.base w 41)))   ;; this form is FINE from outside
  0)
```

```
repro.tur:7:24: error: no typeclass method found for 'base'
```

Note the receiver type does not matter (the non-closure `:int` return above
fails identically), so this is independent of the closure-return carrier work.

## Root-cause direction

Method-dispatch resolution (`.method`) looks the method up against the set of
registered typeclasses/instances. When the call occurs inside a method body of
an instance that is still mid-elaboration, the instance's methods are not yet
visible to the dispatcher, so the lookup for the sibling method fails.

Inspect the `.method` dispatch resolution path in `src/compiler/elab_call.c`
(method-call elaboration) and the point in
`src/compiler/elab_typeclasses.c` where instance method bodies are elaborated
(`elab_definstance`): the sibling methods should be resolvable while elaborating
each body -- either pre-register the instance's method bindings before
elaborating any body, or let `.method` fall back to the class's method table
(the concrete impl is selectable from `self`'s type).

## Validation

- The repro elaborates without error and prints `83` from `(.twice w 41)`
  (`((41*2)+1)`).
- A fixture where one closure-returning method builds its closure by calling a
  sibling method on `self` compiles and runs.

## Workaround in the meantime

Factor the shared logic into a top-level `defn` and call it from both methods,
or compose the methods' results at the call site. The
`tests/fixtures/instance-closure-return-compose-methods` fixture takes the
call-site-composition route for exactly this reason.
