---
title: Cross-module generic-of-generic instantiation: callee not emitted when caller is invoked transitively
category: Codegen / monomorphization gap
severity: Latent miscompile (C-level "undefined function"). Surfaces when a generic function `g` in module B calls a generic function `f` from module A, and the program reaches `g` only through a third-party caller that does not directly reference `f`. The C emitter monomorphizes `g` but does not transitively instantiate `f` for the specialised types, leaving a dangling reference that the C compiler rejects.
description: Pure cross-module generic dispatch breaks when the caller's own caller doesn't directly import or use the callee. The fix surfaced organically: in the calling translation unit, add an explicit direct call to the callee generic at the right type, and instantiation works. That's a workaround, not a fix -- a TU that calls `g(x)` should pull in every monomorphization `g`'s body needs without the user manually anchoring it.
status: OPEN. Surfaced 2026-06-14 while wiring E2c slice 2 (sized storage shapes in turmeric-spices).
---

# Cross-module generic-of-generic instantiation gap

## Summary

A generic function `g [..] [..] : T` whose body calls another generic
`f [..] [..] : U` from a different module fails to compile at the C
level when the only call to `g` is from a translation unit that does
not also directly reference `f`. The emitted C names a function
`f@<concrete-types>` that was never emitted, and `cc` rejects the
TU with `error: call to undeclared function`.

The workaround discovered organically: in the fixture, add a direct
call to `f` at the same concrete types `g` will need. That forces
`f`'s monomorphization into the same TU and the link goes through.

## Minimal repro

Three files, all under `../turmeric-spices/spices/ecs/`:

**`src/ecs/sized-storage.tur`** (module A, the callee module):

```turmeric
(defmodule ecs/sized-storage
  (export SizedDense sized-dense-new sized-dense-len)
(defopaque SizedDense [n A] :int)
(defn __sized-dense-new-raw [k : int] #{Unsafe} : int
  ```c ... ```)
(defn __sized-dense-len-raw [s : int] #{Unsafe} : int
  ```c ... ```)
(defn sized-dense-new [n A] [k : int] : (SizedDense n A)
  (:: (unsafe (__sized-dense-new-raw k)) :SizedDense))
(defn sized-dense-len [n A] [s : (SizedDense n A)] : int
  (unsafe (__sized-dense-len-raw (:: s :int))))
)
```

**`src/ecs/sized-zip.tur`** (module B, the caller module):

```turmeric
(defmodule ecs/sized-zip (export sized-zip-all-three)
(import ecs/sized-storage :refer [SizedDense sized-dense-len])
(defn sized-zip-all-three [n A B]
                          [d : (SizedDense n A) ...] : int
  (sized-dense-len d))   ;; <-- generic call into module A
)
```

**`tests/sized-zip-cross-shape.tur`** (fixture, no direct sized-dense-len call):

```turmeric
(import ecs/sized-storage :refer [SizedDense sized-dense-new
                                   sized-dense-set! sized-dense-free])
(import ecs/sized-zip     :refer [sized-zip-all-three])
(defn main [] : int
  (let [d : (SizedDense (Static 64) int) (sized-dense-new 64)
        ;; ... sparse + tag setup ...]
    (sized-zip-all-three d s t)
    0))
```

`tur run -Xsized-types tests/sized-zip-cross-shape.tur` fails with:

```
tests_sized-zip-cross-shape_tur.c:NNNN: error: call to undeclared
function 'ecs__sized_hystorage__sized_hydense_hylen'; ISO C99 and
later do not support implicit function declarations
    return ecs__sized_hystorage__sized_hydense_hylen(d);
           ^
note: did you mean 'ecs__sized_hystorage__sized_hydense_hynew'?
```

The new/set/free monomorphizations are emitted because the fixture
calls them directly; `sized-dense-len` is not -- the only path to it
is through `sized-zip-all-three`, which the emitter does not follow.

## Workaround that "fixes" the build

Adding a direct call to `sized-dense-len` in the fixture's `main`
(`(println (sized-dense-len d))`) before the `sized-zip-all-three`
call makes the test pass. This anchors `sized-dense-len`'s
monomorphization in the same TU and the cross-shape probe links.

This is the workaround the E2c slice 2 fixture currently uses.

## Why this is the wrong fix

- The whole point of cross-module generics is that the caller's TU
  declares the type and the elaborator instantiates the callee on
  demand. If the user has to manually anchor every transitively-
  called generic, every cross-module generic API becomes a
  documentation hazard: "remember to also call X yourself."
- Real-world callers (the upcoming sized `for-each` macro is a
  prime example) hide many generic-of-generic dispatches behind a
  single user-facing call. The user cannot be expected to know
  which inner generics to also anchor.
- The bug is silent until cc fails -- there is no compiler-level
  diagnostic from `tur`. A passing `tur check` followed by a
  failing `cc` is the worst kind of late failure.

## Suspected root cause

The C emitter walks the program's call graph from `main` and the
module's exported entry points, monomorphizing each generic at the
types it observes. When `g` (in module B) calls `f` (in module A),
the emitter:

1. Records `f@<types>` as needed if it sees `g`'s body during
   `g`'s monomorphization, OR
2. Records `f@<types>` only if it sees a call to `f` in the current
   TU directly.

If the implementation follows path (2), the bug is exactly as
observed: `g` monomorphizes but the inner `f@<types>` reference is
recorded as a forward declaration that never resolves. Path (1) is
the correct behavior.

Confirming this requires reading the monomorphization walker in
`src/compiler/emit_c.c` (or wherever the generic instantiation
worklist lives).

## Proposed fix direction

When monomorphizing a function `g`, the worklist must visit every
generic call inside `g`'s body and enqueue the callee with the
concrete type-arg substitution at `g`'s call site. Today, the
emitter appears to enqueue based on calls in the current
translation unit's source rather than calls reachable through
already-enqueued monomorphizations.

The fix is to teach the worklist to follow generic-call edges
inside the body of every monomorphization it processes, not only
the calls textually present in the current TU.

## Validation of a fix

1. Drop the `sized-dense-len` workaround call from
   `tests/sized-zip-cross-shape.tur` (and the analogous workarounds
   in any other E2c fixture); the test still passes.
2. Add a minimal compiler fixture under
   `tests/fixtures/cross-module-generic-of-generic-accept/`
   reproducing the three-module shape from this report; it must
   pass `tur run` without anchor calls.
3. Confirm no spice in `../turmeric-spices/` regresses.

## Related

- E2c slice 2 commit on branch `e2c/sized-dense` in
  `../turmeric-spices`.
- Design plan: `docs/upcoming/ecs-sized-world-plan.md`.
- Affected fixtures (currently workaround-anchored):
  `spices/ecs/tests/sized-zip-cross-shape.tur`.
