---
title: Existential `pack` of a multi-field struct payload miscompiles -- blocks `world-resize` over sized worlds
category: Compiler / codegen -- existential carrier ABI
severity: Hard error (codegen). `tur check` passes but `emit-c` produces non-compilable C ("aggregate value used where an integer was expected"). No silent miscompile -- it fails loudly at `cc` -- but it is a hard expressiveness wall: the entire by-value sized-world existential surface is unbuildable. Blocks the `ecs-sized-world-plan` `world-resize` helper.
status: RESOLVED 2026-06-17. Fixed in the compiler (this repo) the same session it was filed: `pack` now heap-boxes a by-value aggregate payload and `open` reads it back through the pointer (freeing on the single-use open path), with field access on the opened binding using direct `.field` access. Verified by `tests/fixtures/exists-pack-multifield-struct` (round-trips a `(World m A B)`, leak-clean under ASan/LSan) and a full `bash tests/run.sh` (1676 passed, 0 failed). `world-resize` is unblocked.
---

# Existential `pack` of a multi-field struct payload miscompiles

> **Resolution (2026-06-17).** Fixed in `src/compiler/emit_expr.c`. A new
> predicate `exists_payload_is_byval_aggregate` (keyed on the lowered C name
> via `type_struct_value_c_name`, so it tolerates phantom params defaulted
> to `int64`) gates a heap-boxing path:
> - **pack** (`case EX_EXISTS_PACK`): malloc a copy of the aggregate and
>   carry the pointer instead of casting the struct to `int64`.
> - **open** (`case EX_EXISTS_OPEN`): read the binding back as
>   `*(T *)carrier`, mark the binding `emit_byvalue_carrier_abi = true` so
>   `EX_GET_FIELD` uses direct `.field` access, and `free` the box at the
>   end of the body (single-use ownership, matching the `:linear` open
>   discipline).
>
> Scope of the fix: the **unconstrained** existential path (no typeclass
> witnesses) -- which is what `world-resize` needs. The **constrained**
> aggregate path (`->value = (int64_t)(struct)`, `pack` with witnesses) has
> the same latent issue and would need the rc-record drop hook to free an
> inner box; it is left for a follow-up (no current caller, and the
> world-resize surface is unconstrained). The original analysis is preserved
> verbatim below.

## Summary

Native `pack` boxes its payload into a **single `int64` carrier**. That is
fine for the handle shapes it was validated against (`SizedBuf n`,
`SizedVec n int` -- both one machine word). A `(GameWorld n)` world struct
-- one `(Dense n T)` field per component plus a state cell -- is **wider than
`int64`**, so `pack` emits a C99 cast of an aggregate value to an integer,
which `cc` rejects:

```
error: aggregate value used where an integer was expected
```

This is the blocker behind the `ecs-sized-world-plan` `world-resize`
helper. The plan specs `world-resize` as a thin existential lift that hides
a runtime-chosen capacity behind `(exists [n'] (GameWorld n'))`, built on
`pack-sized` / `open-sized`
([`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur)).
It type-checks but cannot be code-generated.

## Minimal repro

```turmeric
;; A (World m A B) struct mirrors a (defworld GameWorld ...) world:
;; one sized-dense field per component, wider than int64.
(defopaque Dense [n A] :int)

(defstruct World [m A B] (pos (Dense m A)) (vel (Dense m B)))

(defn dn [n A] [v : int] : (Dense n A)
  (:: v :Dense))

(defn main [] : int
  (let [w      (make-struct World (dn 0) (dn 0))
        packed (pack w (exists [m] (World m int int)))]
    (open packed [m w2]
      (println 0)))
  0)
```

- `tur check` -- **passes** (the existential is well-typed).
- `tur emit-c` -- exits 0 but emits invalid C at the pack site:

  ```c
  World__int__int__int w_904 =
      (World__int__int__int){.pos = dn(INT64_C(0)), .vel = dn(INT64_C(0))};
  ...
  void * packed_905 = (tur_exists_t)(intptr_t)((int64_t)(w_904));   // <- aggregate -> int64
  ```

- `cc -c out.c` -- **fails**:

  ```
  error: aggregate value used where an integer was expected
  ```

This is exactly the prototype failure described against the real
`make-GameWorld`: the struct initializer is assigned to an `int64_t`
carrier, and `pack` reinterpret-casts the aggregate to an integer.

## Root cause

`pack` lowering in
[`src/compiler/emit_expr.c`](../../src/compiler/emit_expr.c) `case
EX_EXISTS_PACK:` (around line 4865) boxes the payload into one `int64`
slot on **both** of its paths:

- **Unconstrained** existential (the sized-world case -- no typeclass
  witnesses), `emit_expr.c:4949`:

  ```c
  /* Scalar (int64_t, bool, etc.) -- reinterpret via intptr_t (64-bit safe) */
  buf_printf(&out, "(tur_exists_t)(intptr_t)((int64_t)(%s))", val);
  ```

  A struct value's `TypeKind` is neither `TY_PTR_VOID`/`TY_EXISTS`/`TY_FORALL`
  (the "already a pointer" branch at `emit_expr.c:4946`) nor a true scalar,
  so it falls into this `(int64_t)(struct)` cast -- the invalid C.

- **Constrained** existential (`n_witnesses > 0`), `emit_expr.c` ~4925:

  ```c
  buf_printf(body, "%s->value = (int64_t)(%s);\n", rec_tmp, val);
  ```

  stores the payload into `tur_existential_t.value`, also a single
  `int64_t`.

The carrier type itself is one machine word in both directions
([`src/compiler/emit_module.c:3300`](../../src/compiler/emit_module.c)):

```c
typedef void * tur_exists_t;                 /* unconstrained: 1 word     */

typedef struct tur_existential {             /* constrained record         */
    int64_t  value;                          /*   <- single-word payload   */
    int32_t  n_witnesses;
    void    *witnesses[];
} tur_existential_t;
```

So the existential ABI assumes every packed payload fits in 8 bytes. It was
only ever validated against single-int handles (`SizedBuf`, `SizedVec`), per
the module docstring at the top of
[`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur)
("Validated against both `(SizedVec n int)` ... and `(SizedBuf n)`"). The
`ecs-spice-plan` calls out the same carrier assumption explicitly
([`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md), E2d-P6
"Out of scope"):

> Today every storage handle rides the `int64` carrier so this is fine; if
> a backend ever wants non-int handles, that compiler work has to land
> first.

A by-value `(GameWorld n)` struct is precisely that "non-int handle."

## Observed vs. expected

- **Observed:** `pack` of a struct payload emits `(tur_exists_t)(intptr_t)((int64_t)(aggregate))`;
  `cc` errors with "aggregate value used where an integer was expected."
- **Expected:** `pack` of a payload wider than `int64` heap-boxes the
  aggregate (or carries a pointer to it) so the carrier holds a pointer, not
  a truncated/illegal integer cast; `open` reads it back through that
  pointer. The build succeeds and the round-tripped world is structurally
  intact.

## Proposed fix directions

1. **Heap-box wide payloads in `pack` (smallest change).** When the packed
   value's resolved type is a heap/aggregate struct (reuse the
   `type_is_heap_struct` / `type_has_concrete_codegen_layout` predicates
   already used by the carrier-bridge path a few lines above the pack case
   in `emit_expr.c`), `malloc` a copy of the aggregate and store the pointer
   in the carrier; `open` casts back to `T*` and dereferences. Mirrors how
   constrained existentials already heap-allocate their record.
   - For the unconstrained path this means the carrier holds a real
     `void*`; for the constrained path, `tur_existential_t.value` already
     holds an `int64_t` that can carry the pointer -- only the store/load
     casts change (no struct-layout change to `tur_existential_t`).
   - Ownership: a heap-boxed pack needs the same drop discipline the
     constrained `:linear` / rc paths already implement (`EX_EXISTS_OPEN`
     frees the linear record at end-of-body); extend that to the boxed
     unconstrained case.

2. **Struct-carrier existentials (more invasive).** Generalize
   `tur_exists_t` / `tur_existential_t` to carry a sized payload inline
   (flexible array sized to the monomorph's struct width). Avoids the extra
   allocation but touches every pack/open emit site and the cycle walker.

Direction 1 is the recommended first cut -- it is local to the pack/open
emit sites, reuses the existing heap-struct predicates and the
linear/rc drop machinery, and unblocks `world-resize` without an ABI change
to the existential record.

## How to validate a fix

1. The repro above (`/tmp/repro/world-pack.tur` in the reporting session;
   reproduced verbatim under "Minimal repro") must `tur emit-c | cc -c`
   cleanly and round-trip: open the packed world, read a field, confirm the
   value survives.
2. Add a fixture under `tests/fixtures/` -- e.g.
   `exists-pack-multifield-struct/` -- packing a multi-field
   `(World m A B)` and opening it, with an `expected.c` snapshot and a
   runtime assertion on a projected field. Pair it with an error/negative
   fixture confirming the escape checks (open binder must not leak) still
   fire for struct payloads.
3. Re-run `bash tests/run.sh` (leak detection ON) and confirm zero `FAIL`
   and no new ASan/LSan reports -- a heap-boxing pack must free on the
   open-side path.
4. Once landed, implement `world-resize` per
   [`docs/upcoming/ecs-sized-world-plan.md`](../upcoming/ecs-sized-world-plan.md)
   (Resize section) and confirm a runtime-chosen `n'` round-trips through
   `(exists [n'] (GameWorld n'))`.

## Related

- [`docs/upcoming/ecs-sized-world-plan.md`](../upcoming/ecs-sized-world-plan.md)
  -- the `world-resize` helper this blocks (Resize / "out-of-band, via
  existential" section).
- [`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur)
  -- `pack-sized` / `open-sized`, validated only against single-int handles.
- [`docs/reported/sized-scheduler-system-stage-world-carrier.md`](sized-scheduler-system-stage-world-carrier.md)
  -- the sibling blocker (Sized scheduler over sized worlds); **same int64
  carrier wall**, different surface.
- [`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md) E2d-P6
  "Out of scope" note -- the codebase already flags this exact carrier
  limitation.
