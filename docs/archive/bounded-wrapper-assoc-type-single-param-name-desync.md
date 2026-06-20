---
title: Bounded `[S] [(StorageOps S)]` wrapper desyncs the instance-method symbol for the single-param + associated-type shape (`(Dense A)` head)
category: Typeclass instance mangling / bounded-wrapper monomorphisation -- link failure
severity: Medium. A bounded wrapper quantified over a single-parameter class
  with an associated type (`(defclass StorageOps [S] (type Elem : Type) ...)`,
  `(definstance StorageOps [(Dense A)] (type Elem = A) ...)`) type-checks but
  fails to LINK: the wrapper's interior dispatch call is reconstructed as
  `__inst_StorageOps_storage_hyhas_qu_Dense` (receiver head constructor only)
  while the parametric instance is emitted under the mangler's `(Dense A)`
  suffix `__inst_StorageOps_storage_hyhas_qu_Dense__ltstruct_gt` (the free tyvar
  element rendered via the legacy `<struct>` token). Undefined reference at cc.
status: RESOLVED -- see "Resolution".
---

# Bounded wrapper name-desync for the single-param + associated-type shape

## One-line summary

`#447` closed gap H for the multi-parameter *ground* instance head
(`StorageOps [(Dense Pos) Pos]`) by resolving the authoritative instance-method
binding name via `emit_concrete_inst_method_name`
(`src/compiler/emit_core.c`). That helper matched the instance by strict
`type_eq(inst->type_args[j], resolved)`, which a *parametric* head `(Dense A)`
can never satisfy against the concrete receiver `(Dense Pos)`. So for the
single-param + associated-type shape the lookup returned NULL and the caller
fell back to single-component reconstruction (`_Dense`), desyncing from the
instance's emitted suffix (`_Dense__ltstruct_gt`).

## Minimal repro

```turmeric
(extern-c printf [^cstr fmt ^int v] : int)
(defopaque Dense [A] :int)
(defstruct Pos [x : int])
(defn dense-pos [] : (Dense Pos)  ```c return 0; ```)

(defclass StorageOps [S]
  (type Elem : Type)
  (storage-get  [^borrow s : S idx : int] : Elem)
  (storage-has? [^borrow s : S idx : int] : bool))

(definstance StorageOps [(Dense A)]
  (type Elem = A)
  (storage-get  [s idx] ```c (void)s; return (int64_t)(idx + 100); ```)
  (storage-has? [s idx] ```c (void)s; return idx == 7; ```))

(defn any-has? [S] [(StorageOps S)] [^borrow s : S idx : int] : bool
  (storage-has? s idx))

(defn main [] : int
  (let [d : (Dense Pos)  (dense-pos)]
    (printf "has7=%d\n" (if (any-has? d 7) 1 0))
    0))
```

```
$ ./build/tur build /tmp/sop.tur -o /tmp/sop
... warning: implicit declaration of '__inst_StorageOps_storage_hyhas_qu_Dense';
    did you mean '__inst_StorageOps_storage_hyhas_qu_Dense__ltstruct_gt'?
/usr/bin/ld: undefined reference to '__inst_StorageOps_storage_hyhas_qu_Dense'
```

## Root cause (file:line)

`src/compiler/emit_core.c`, `emit_concrete_inst_method_name`: the instance
match used `type_eq(inst->type_args[j], resolved)`. The parametric instance
head's `type_args[0]` is `TY_APP(Dense, TYVAR A)`; the resolved receiver is
`TY_APP(Dense, Pos)`. `type_eq` compares the TY_APP args structurally
(`types.c` TY_APP arm), so `TYVAR A` vs `Pos` makes them unequal, the helper
returns NULL, and `emit_reresolve_method_call` falls through to
single-component reconstruction (`__inst_<Class>_<method>_<head>` =
`..._Dense`). The instance itself was emitted under
`build_inst_type_suffix`'s `(Dense A)` suffix `_Dense__ltstruct_gt` (the free
tyvar element rendered as the legacy `<struct>` token; see
`docs/reported/instance-suffix-mangler-tyvar-element-legacy-struct-token.md`).

## Resolution

`emit_concrete_inst_method_name` now matches the instance head with a new
`emit_inst_head_matches(pattern, concrete)` that treats a `TY_TYVAR` in the
instance-head pattern as a wildcard (head constructors are still compared by
identity, recursing through `TY_APP`). So `(Dense A)` matches `(Dense Pos)`,
the helper finds the instance, and returns its method impl's authoritative
FnDef binding name -- the same spelling `EX_INSTANCE_DEF` wires into the dict
singleton -- keeping the wrapper's call site and the emitted impl in lockstep
regardless of how the parametric suffix is mangled.

This fixes the desync at the consumption site without touching the mangler, so
the cosmetic `<struct>`-token smell (the open report above) is unaffected: the
instance and the wrapper now agree on whatever spelling the mangler produces.

**Validation:** the repro links, runs, and prints `has7=1`/`has9=0`. New
regression fixture
`tests/fixtures/typeclass-bounded-wrapper-assoc-type-dispatch/`. Full suite:
1692 passed, 0 failed.

## Related

- `docs/archive/bounded-storageops-wrapper-heterogeneous-monomorphisation-gap.md`
  (gap H -- the multi-parameter ground-head half, fixed in #447).
- `docs/reported/instance-suffix-mangler-tyvar-element-legacy-struct-token.md`
  (the `<struct>`-token mangling smell that determines the parametric suffix).
- `src/compiler/emit_core.c` -- `emit_inst_head_matches`,
  `emit_concrete_inst_method_name`, `emit_reresolve_method_call`.
