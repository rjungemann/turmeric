---
title: (unsafe ...) block fails to capture variables used inside an ascription
category: Reported
description: An `(unsafe ...)` block is lowered to a fiber/handler body whose free variables are captured into a heap env struct. The capture scan does not descend into `(:: expr type)` ascription nodes, so a variable referenced only inside an ascription within the unsafe block is omitted from the env -- the emitted body references an undeclared C local and fails to compile.
---

# `(unsafe ...)` block drops ascription-wrapped captured variables

> **Severity:** silent-miscompile -> hard cc error ("undeclared identifier").
> Easy to trip and the message points at generated C, not the source.
> **Found:** 2026-06-04, executing
> [stdlib-session-typed-channels-plan](../upcoming/stdlib-session-typed-channels-plan.md)
> (the typed wrappers call inline-C raw ops from inside `(unsafe ...)`).

## Summary

`(unsafe ...)` is lowered to a fiber body (`__handle_body_N` + `__HEnv_N`).
The free-variable scan that builds `__HEnv_N` does not look inside
`(:: expr type)` ascriptions, so a captured variable that appears only inside an
ascription in the unsafe block is never copied into the env. The generated body
then references the bare C local (e.g. `v_858`) which does not exist in that
function, and `cc` fails with "undeclared identifier".

## Minimal repro

```turmeric
(defmodule m
  (export f)
  (defn raw [a : ptr<void> b : int] : nil ```c (void)a; (void)b; ```)
  ;; v is referenced only through (:: v :int) inside the unsafe block
  (defn f [x : ptr<void> v : int] : nil (unsafe (raw x (:: v :int))))
  (defn main [] : int 0))
```

### Observed

```
error: 'v_858' undeclared (first use in this function)
```

The emitted `__handle_body` captures `x` (referenced bare) into the env but not
`v` (referenced only inside `(:: v :int)`), then emits `... , v_858)` against a
local that was never declared in the handler body.

### Expected

`v` is captured into `__HEnv` like `x`, and the unsafe block compiles. Passing
`v` *without* the ascription -- `(unsafe (raw x v))` -- already captures
correctly, confirming the gap is specifically the ascription node.

## Root cause (analysis)

`(unsafe ...)` lowering (the fiber/handler env builder; see the
`__HEnv`/`__handle_body`/`tur_fiber_block_new` emission for unsafe blocks) walks
the block body collecting free variables to copy into the heap env. The walk
does not recurse through `EX_ASCRIBE` / `(::)` (and likely the related
`EX_REINTERPRET`) wrapper nodes, so variables that occur only under an
ascription are missed. (The matching emit step *does* emit the inner reference,
producing the use of an undeclared local.)

## Impact / workaround

- In `stdlib/schan.tur` every `(unsafe (... (:: v :int) ...))` was rewritten to
  lift the ascription into a `let` binding outside the unsafe block, so the
  unsafe block only references plain locals:

  ```turmeric
  (let [vi (:: v :int)]
    (unsafe (raw x vi)))
  ```

## Proposed fix directions

1. In the unsafe-block free-variable capture scan, recurse into `EX_ASCRIBE`
   (and `EX_REINTERPRET`) sub-expressions so their variable references are
   collected into the env.
2. Audit other capture scanners (closures, fiber/async bodies) for the same
   "stops at ascription" gap.

## Validation

The repro above should build and run (exit 0). More generally, any captured
variable used only inside `(:: v T)` within an `(unsafe ...)` block should be
copied into the env and the generated body should compile.
