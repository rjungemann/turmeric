---
title: `any` box type-ids are assigned per translation unit, so `type-of` / `is?` / `cast` / drop all disagree across a module boundary in a multi-TU build
category: Reported
description: emit_any_type_id interns names into the per-TU EmitCtx and hands out TUR_ANY_ID_BASE + first-seen index, so the same type gets a different id in each TU. Under `tur build --shared` (multi-TU) a value widened to `any` in one module is misidentified in another -- type-of returns another type's name, is? is a false negative, a valid cast panics naming the wrong type, and __tur_any_drop consults the wrong boxed flag. `tur build <dir>` is single-TU and hides all four.
---

# `any` box type-ids are per-TU, and multi-TU builds disagree

**Severity: high, low reach today.** Four distinct wrong behaviours including a
memory-management one, all on a shipping build mode (`tur build --shared`). The
reach is small only because `any` across a module boundary is rare in typed
Turmeric. It stops being rare the moment anything makes `any` common -- which
is exactly what `#lang saffron` would do, so this is filed as a prerequisite
for [docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) S0
rather than as a curiosity.

## Root cause

`emit_any_type_id` (`src/compiler/emit_module.c:752`) interns a named type's
`type_name` into `ctx->any_type_names[]` and returns
`TUR_ANY_ID_BASE + index` -- the **first-seen index within one `EmitCtx`**:

```c
for (uint32_t i = 0; i < ctx->n_any_type_names; i++) {
    if (strcmp(ctx->any_type_names[i], key) == 0)
        return (int64_t)(TUR_ANY_ID_BASE + i);
}
...
return (int64_t)(TUR_ANY_ID_BASE + ctx->n_any_type_names - 1);
```

`EmitCtx` is per translation unit -- `memset(&ctx, 0, sizeof(ctx))` at
`emit_module.c:13604` and `:16145`. So the id a type gets depends on the order
in which *that TU* happened to widen types, and two TUs in one program assign
different ids to the same type.

Three things then key off an id that is not agreed:

- `__tur_any_name_ext` -- a `static` per-TU switch, each installed into the
  single global `g_tur_any_name_ext` from its own `__tur_static_init`. Last
  writer wins; every other TU's ids are then read through the wrong table.
- `is?` and `cast` -- both emit a literal id compare against the *emitting*
  TU's numbering.
- `__tur_any_drop` (`emit_module.c:818`) -- a per-TU switch over the ids whose
  payload the *emitting* TU heap-boxed.

## Repro (v0.44.2, `2da89e84`)

`src/amod.tur`:

```turmeric
(defmodule amod
  (export make-beta Beta)
  (defstruct Alpha [a : int])
  (defstruct Beta  [b : int])
  (defn make-beta [] : any (make-struct Beta 7)))
```

`src/main.tur` -- interns `Gamma` **before** calling into `amod`, so its local
numbering differs:

```turmeric
(defmodule main-mod
  (import amod :refer [make-beta Beta])
  (defstruct Gamma [g : int])
  (defn wrap-gamma [] : any (make-struct Gamma 1))
  (defn main [] : int
    (let [g (wrap-gamma)] (println (type-of g)))
    (let [v (make-beta)]
      (println (type-of v))
      (println (if (is? v Beta) 1 0)))
    0))
```

```
$ tur build . && ./build/bin/anyids        # single-TU
Gamma
Beta
1                                          # correct

$ tur build --shared .                     # multi-TU; link the obj/*.c and run
Gamma
Gamma                                      # WRONG -- a Beta reports as "Gamma"
0                                          # WRONG -- (is? v Beta) is false
```

The emitted C says exactly why:

```c
/* obj/amod.c */
case 1000: return "Beta";
return ({ ... TUR_TAG(1000, (int64_t)(intptr_t)__tur_box); });   /* Beta -> 1000 */

/* obj/main.c */
case 1000: return "Gamma";
case 1001: return "Beta";
if ((TUR_GETTAG(v_1445) == 1001)) { ... }                        /* is? v Beta */
```

`amod` tags a `Beta` **1000**; `main` tests for `Beta` at **1001** and reads
tag 1000 as `"Gamma"`.

### Failure mode 3 -- a valid `cast` panics, naming the wrong type

Same project, `main` casting the value it received:

```
$ ./app
panic at tur_runtime.h:1817: cast: any holds ByVal, not HeapThing
```

The value *is* a `HeapThing`. Both type names in the message are wrong: the
"holds" name comes from reading the foreign tag through the local table.

### Failure mode 4 -- `__tur_any_drop` consults the wrong boxed flag

`emit_any_type_id` records, per id, whether the widen site heap-boxed the
payload (`ctx->any_type_boxed[i]`, from `emit_type_is_byvalue_adt`). That flag
is per-TU too, so an id can be boxed in one TU and not in another. Constructed:

```turmeric
;; amod: a :heap struct -- the handle rides the value word, NOT boxed
(defstruct HeapThing :heap [x : int])
(defn mk-heap [] : any (make-struct HeapThing 7))

;; main: a by-value struct -- heap-boxed on widen
(defstruct ByVal [a : int])
(defn mk-byval [] : any (make-struct ByVal 1))
```

```c
/* obj/amod.c -- HeapThing = 1000, unboxed */
return TUR_TAG(1000, (int64_t)(intptr_t)(__ps_180));
static void __tur_any_drop(tur_tagged_t __v) {
    switch (TUR_GETTAG(__v)) { default: return; }      /* frees nothing */
}

/* obj/main.c -- ByVal = 1000, boxed */
static void __tur_any_drop(tur_tagged_t __v) {
    switch (TUR_GETTAG(__v)) {
        case 1000: free((void *)(intptr_t)TUR_UNTAG(__v)); return;
        default: return;
    }
}
```

and `main`'s body does emit the drop on the foreign value:

```c
tur_tagged_t h_1443 = __ps_182;                 /* from amod: tag 1000, a live handle */
puts(__tur_any_type_name(TUR_GETTAG(h_1443)));
__tur_any_drop(h_1443);                         /* free()s a handle main does not own */
```

So `main` calls `free()` on `amod`'s live `HeapThing` handle -- a premature
free of memory this TU never allocated. The mirror case (a boxed payload
reaching a TU whose table says that id is unboxed) leaks instead.

**Not observed as a crash.** I did not get an ASan hit on this one: the shapes
that would read the handle after the drop hit the failure-mode-3 cast panic
first. The claim here is from the emitted C, which is unambiguous about both
halves -- the `free()` is emitted, and the pointer belongs to the other TU.

## Why `tur build <dir>` hides it

Project mode folds the whole program into **one** TU. Verified by wrapping
`CC`: `tur build .` on the two-module project passes exactly one `.c`
(`/tmp/tur-build/..._main_tur.c`, 7445 lines, both modules inside), with one
consistent table (`Beta` 1000, `Gamma` 1001).

`tur build --shared .` passes three (`obj/amod.c`, `obj/main.c`,
`obj/tur_runtime.c`), and that is where the ids diverge. `tur emit-c
--output-dir` -- the documented CMake-consumer path -- produces the same split.

The `.tur-abi-cache/` does not participate: it carries no `any` id
information, so there is no existing coordination point to extend.

## Fix directions

1. **Make the id a deterministic function of the type, not of intern order.**
   A 64-bit hash of `type_name` (offset clear of the `TypeKind` range, which
   the primitives still use) needs no coordination between TUs and survives
   separate compilation, `--shared`, and the CMake path alike. This is the
   recommended direction.

2. **Then `__tur_any_name_ext` has to become a registry, not a switch.** With
   hashed ids each TU can only name the types *it* mentions, so `type-of` on a
   foreign tag would answer `"unknown"`. Have each TU register its
   `(id, name, boxed)` rows into a global table from `__tur_static_init` and
   have lookups consult the union. `g_tur_any_name_ext` is already a global
   hook -- it becomes a global table instead of a single function pointer.
   The `boxed` flag must travel in the same row, which fixes failure mode 4 by
   construction rather than separately.

3. **Rejected: a link-time section table.** Portable-ish on ELF, but the tree
   targets Windows and macOS too, and this does not need the complexity.

4. **Rejected: whole-program id assignment.** It would defeat separate
   compilation, which is the case that is broken.

A fixture belongs on the multi-TU path specifically -- every existing `any`
fixture is single-TU, which is why four wrong behaviours went unnoticed. The
cheapest shape is a two-module project built `--shared`, asserting `type-of`,
`is?`, and a round-trip `cast` across the boundary.
