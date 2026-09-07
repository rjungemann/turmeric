---
title: `any` box type-ids are assigned per translation unit, so `type-of` / `is?` / `cast` / drop all disagree across a module boundary in a multi-TU build
category: Archive
description: RESOLVED 2026-09-07. emit_any_type_id interns names into the per-TU EmitCtx and hands out TUR_ANY_ID_BASE + first-seen index, so the same type gets a different id in each TU. Under `tur build --shared` (multi-TU) a value widened to `any` in one module is misidentified in another -- type-of returns another type's name, is? is a false negative, a valid cast panics naming the wrong type, and __tur_any_drop consults the wrong boxed flag. `tur build <dir>` is single-TU and hides all four.
---

# `any` box type-ids are per-TU, and multi-TU builds disagree

**RESOLVED 2026-09-07**, along fix directions 1 and 2 as filed; 3 and 4 stayed
rejected for the reasons recorded below.

**The id is now a hash of the identity key**, not this TU's intern index --
FNV-1a over `type_name`, forced into the top quarter of the positive range so it
can never be confused with the `TypeKind` a primitive payload still tags with.
No coordination between TUs is required, so it survives separate compilation,
`--shared`, and the CMake path alike. A collision between two distinct keys
would silently make one type answer as another, so `emit_any_type_id` aborts on
one within a TU; across TUs it is ~2^-62 and unobservable, which is the accepted
cost of not coordinating.

**`__tur_any_name_ext` became a registry**, as direction 2 required. It had been
a single `g_tur_any_name_ext` function pointer that every TU overwrote from its
own static initializer -- genuinely shared under `TUR_RT_OWNER`, so last writer
won and every other TU's ids were then read through the wrong table. Each TU now
publishes a `{ id, name, boxed }` row array and links it into a global chunk
list at static-init; lookups walk the union.

**Failure mode 4 is fixed by construction**, not separately: the `boxed` flag
rides the same row as the name, so the drop site reads the flag the *minting* TU
published. `__tur_any_drop` is now a registry lookup rather than a switch over
the local TU's ids -- which is what let one module `free()` a handle another
module owned.

**Measured on the original repro.** Multi-TU went from `Gamma / Gamma / 0` to
`Gamma / Beta / 1`; the boxed-flag case went from a `HeapThing` reporting as
`ByVal` (with a `free()` of a foreign handle emitted) to both TUs agreeing on
`{ 7338711515630455999LL, "HeapThing", 0 }`.

**Pinned by `tests/run-any-type-id-multi-module.sh`** (ctest
`tur_any_type_id_multi_module`), which asserts both TUs emit an identical
`(id, name, boxed)` row for a shared type, links and runs the genuinely
multi-TU program, and runs the whole-program build too so a future change
cannot fix one build mode by breaking the other. Verified to FAIL without the
fix: 4 of 9 assertions, with all four behaviours visible (`type-of` answering
`Alpha` for a `Gamma`, `is?` returning 0, and a `cast: any holds Beta, not
HeapThing` panic). The fixture deliberately widens *two* local types before
touching the imported module -- with one, the indices happened to line up from
`Beta` onward and only mode 1 tripped.

**One cost, recorded.** `__tur_any_find` is linear over (chunks x rows).
Programs intern a handful of `any` types so this is a short walk, but
`__tur_any_drop` calls it at every scope exit owning an `any`. If that ever
measures, the fix is an index built once at startup -- not a return to per-TU
numbering.

**Suites at the fix:** `run.sh` 2832/0, `run-turi.sh` 1926/0,
`run-leak-check.sh` 83/0, `run-build-shared.sh` 11/0. All 148 `expected.c`
snapshots regenerated in the same change.

---

## The original report

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
