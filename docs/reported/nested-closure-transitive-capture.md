---
title: Nested Closures Fail to Transitively Capture Grandparent Variables
category: Reported Bug
description: A closure nested two levels deep that references a variable bound in its grandparent scope emits C that references an undeclared local (the variable is never threaded into the middle closure's environment). The generated C fails to compile. Affects plain `defn` as well as instance methods; orthogonal to the dict-field carrier type.
---

# Nested Closures Fail to Transitively Capture Grandparent Variables -- Reported Bug

> **Status:** OPEN (found 2026-06-03)
> **Severity:** High -- a hard C **compile error** (`'n_854' undeclared`), not a
>   warning. Any genuinely curried closure whose innermost body reads an
>   outermost binding fails to build. Latent because most curried closures in
>   the tree only reference their immediate parent's parameter.
> **Found:** while executing
>   [closure-returning-instance-method-codegen-plan](../upcoming/closure-returning-instance-method-codegen-plan.md);
>   surfaced by the T6.4 "nested" coverage fixture.
> **Scope:** general closure codegen -- reproduces in a plain `defn`, with no
>   typeclasses or `definstance` involved. It is **not** the dict-field carrier
>   bug fixed by that plan (that fix landed; the curried *return type* now
>   lowers to `int64_t` at all three sites). This is a separate capture-set
>   threading defect.

## Summary

A three-level curried closure where the **innermost** closure references a
variable bound by the **outermost** function (skipping the middle closure) does
not thread that variable through the middle closure's environment. The middle
closure's generated constructor references the variable by its mangled local
name, but that name is not in scope there -- it lives in the outer function's
frame, which the middle closure never captured.

## Minimal repro

```turmeric
(defn adder [n :int] : (fn [:int] (fn [:int] :int))
  (fn [x :int] : (fn [:int] :int)
    (fn [y :int] :int (+ (+ x y) n))))   ;; inner reads x (parent) AND n (grandparent)
(defn main [] :int 0)
```

```sh
$ ./build/tur emit-c repro.tur > repro.c && cc -std=c99 -c repro.c
repro.c:3193:20: error: 'n_854' undeclared (first use in this function)
```

### Observed vs. expected

- **Observed:** the middle closure's constructor (`__fn_<mid>`) emits
  `__env->n = n_854;` (threading `n` into the *inner* closure's env) but
  `n_854` is not a parameter or local of `__fn_<mid>` -- it is the outer
  `adder`'s parameter. The middle closure captured `x` but never captured `n`,
  so `n` is unavailable to forward. C fails to compile.
- **Expected:** `n` is part of the middle closure's capture set (because a
  closure it builds needs it), so the middle env carries `n` and forwards it to
  the inner env. The program builds and `((adder 100) 20) 3 == 123`.

## Root-cause direction

The free-variable / capture-set analysis computes a closure's captures from the
variables its *own* body mentions, but does not take the transitive closure
over variables needed by *nested* closures it constructs. A variable read only
by a grandchild closure must still be captured by every intermediate closure on
the path from its binder, so each frame can forward it inward.

Likely sites to inspect: the closure free-variable collection pass (where
`EX_FN` capture sets are computed) and the env-struct/env-forwarding emission in
`src/compiler/emit_fns.c` / `emit_expr.c`. The fix is to propagate a nested
closure's capture requirements up to each enclosing closure (union the child's
free-set, minus the child's own params, into the parent's capture set).

## Validation

- The repro above compiles cleanly and prints `123` from
  `((adder 100) 20) 3`.
- A regression fixture exercising transitive capture through two closure levels
  passes end-to-end.

## Workaround in the meantime

Curried closures whose inner bodies reference only their immediate parent's
parameters work today. The
`tests/fixtures/instance-closure-return-nested` coverage fixture is
deliberately written this way (the inner closure reads only `x`, never a
grandparent binding) so it exercises the curried *return-type* carrier fix
without tripping this orthogonal capture bug.
