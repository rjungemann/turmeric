---
title: "Malformed `defdata` constructor field type crashes (NULL-deref in `elab_lookup_ctor`) once the constructor is referenced"
category: Elaboration / error recovery -- `defdata` registers a partial AdtDef with a NULL ctor slot on a field-type error
severity: Medium. A compiler **crash** (ASan NULL-deref SEGV, or a wild deref in
  a non-sanitized build) on malformed source, after the correct diagnostics
  have already been printed. It is a bad-input robustness hole, not a
  miscompile of valid code -- but `tur check`/`tur build` should report the
  error and exit, never segfault. Easy to hit from a typo (`:int` written as
  `int`).
status: OPEN
reported-by: Claude (noticed while testing the S6 fix; see docs/archive/self-recursive-call-typed-at-carrier-int.md)
verified-on: turmeric 0.21.0, this tree (post #459 / #460)
---

# Malformed `defdata` field type -> NULL-deref in `elab_lookup_ctor`

## One-line summary

A `defdata` whose constructor has a malformed field type (e.g. a bare `int`
instead of the keyword `:int`) emits the correct diagnostic but leaves the
already-registered `AdtDef` with `n_ctors` counting a constructor slot that was
never filled (`ctors[ci] == NULL`). The next reference to that constructor walks
`elab_lookup_ctor`, which dereferences `adt->ctors[ci]->name` without a NULL
check, and the compiler crashes.

## Reproduction (verified on this tree)

```turmeric
(defdata Acc (MkAcc int))                 ;; `int`, not `:int` -- malformed
(defn mk [n : int] : Acc (MkAcc n))
(defn main [] : int (let [a (mk 3)] 0))
```

```
ddmin.tur:1:21: error: defdata: constructor field type must be a keyword like :int, :bool, :cstr
ddmin.tur:2:27: error: unknown function or operator 'MkAcc'
src/compiler/elab_structs.c:2330:17: runtime error: member access within null pointer of type 'struct CtorDef'
AddressSanitizer: SEGV on unknown address 0x000000000000
    #0 ... in elab_lookup_ctor src/compiler/elab_structs.c:2330
```

The two diagnostics are correct; the crash happens **after** them, when a later
phase resolves the `Acc`-returning `mk` (the call/let-binding of `(mk 3)` is
what tips it into `elab_lookup_ctor`). A bare `(defdata Acc (MkAcc int))` with
no use of `MkAcc`/`Acc` does **not** crash -- the corrupt AdtDef is only walked
once something looks a constructor up.

## Root cause (file:line)

1. `elab_defdata` registers the `AdtDef` into `e->adt_defs` early
   (`src/compiler/elab_structs.c:1201`) and allocates the ctor array with the
   form-derived count (`def->n_ctors = n_ctors; def->ctors = arena_alloc(...)`
   at `:1311-1312` / `:1343-1344`). The array is zero-initialized, so every slot
   starts NULL.
2. Each ctor is filled in the field loop and stored at the **bottom** of the
   loop: `def->ctors[ci] = ctor;` (`:1473`).
3. On a malformed field type the loop bails early --
   `diag_emit(...); return NULL;` (`:1440-1443`) -- **before** reaching the
   `def->ctors[ci] = ctor` store. So the AdtDef stays in `e->adt_defs` with
   `n_ctors == 1` and `ctors[0] == NULL`.
4. `elab_lookup_ctor` iterates every registered ADT's ctors and dereferences
   `adt->ctors[ci]->name` with no NULL guard
   (`src/compiler/elab_structs.c:2330`):

   ```c
   CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name) {
       for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
           AdtDef *adt = e->adt_defs[ai];
           for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
               if (strcmp(adt->ctors[ci]->name, name->name) == 0) {  // ctors[ci] == NULL
                   return adt->ctors[ci];
               }
           }
       }
       return NULL;
   }
   ```

## Fix directions

Either (preferably both):

1. **Null-guard the lookup (cheap, robust).** In `elab_lookup_ctor`, skip a
   NULL `adt->ctors[ci]` (and a NULL `->name`) before the `strcmp`. This turns
   the crash into clean recovery -- the two real diagnostics are already
   emitted and the run exits non-zero. One-line robustness fix that also
   defends every other `e->adt_defs` walker against a partially-built AdtDef.

2. **Don't leave a partial AdtDef registered.** On the field-type error at
   `:1440-1443` (and the `unrecognized type` error at `:1458-1460`), either
   roll the AdtDef back out of `e->adt_defs` / `e->n_adt_defs`, set
   `def->n_ctors` to the number of slots actually filled, or flag the AdtDef
   invalid so lookups skip it. This keeps the registry's invariant
   "`ci < n_ctors` implies `ctors[ci] != NULL`" that the rest of the code
   assumes.

## Notes / scope

- Pure bad-input robustness; no valid program is affected.
- Same shape would bite the `unrecognized type :%s` error path
  (`:1458-1460`), which also `return NULL`s after the early registration.
- The minimal repro needs the malformed constructor to actually be *looked up*
  later (a defn that returns or constructs the type, then is used); the bare
  `defdata` alone is harmless.
