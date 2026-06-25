---
title: A multi-type-param struct annotation `(Map K V)` lowers to a spineless (degenerate) TY_APP in `fd->param_types` / `fd->return_type`, defeating every spine-walking predicate
category: Bug report / latent representation defect -- Elaboration -> ABI / Codegen (end-to-end monomorphization)
severity: Low-Medium. Not a miscompile today -- every site that needs the
  StructDef carries a fallback (the carrier-forcing block keys on the resolved
  type; the MutableMap/Map producer-typing leans on `__TUR_RET__`). But it is a
  latent footgun: any predicate that walks the declared type's spine
  (`type_extract_struct_app` / `type_is_heap_struct` / `type_is_heap_vec`)
  silently returns false for a `(Map K V)` / `(MutableMap K V)` parameter or
  return, so each new consumer of a multi-param `:heap` collection type must
  rediscover this and add its own resolved-type workaround.
---

# Multi-param struct annotations lose their spine in `fd->param_types`

## RESOLVED (2026-06-25)

Fixed end-to-end. The realiser (`src/compiler/elab_fns.c`) now preserves the
spined `type_app` chain for a multi-param `(Map K V)` / `(MutableMap K V)`
parameter (new `TY_APP` preserve case, mirroring the `TY_STRUCT` one) and for a
multi-param return (using the already-captured `return_app_type`). With a real
spine, `type_extract_struct_app` / `type_is_heap_struct` / `type_is_heap_vec`
extract the head StructDef directly from the *declared* type, exactly like the
single-param `(Vec A)` slice always did.

The cross-cutting audit (step 2) found the only reader that genuinely depended
on the spineless shell was the `::` (`EX_REINTERPRET`) call-site lowering in
`src/compiler/emit_expr.c`: its KB-015 second loop unconditionally skipped the
carrier reinterpret for any specialized call, assuming the clone returned the
concrete type. Once multi-param accessors (`map-get-eq-o : V`) started minting
typed specs, that assumption broke -- those specs keep the int64 carrier as
their `result_type` (forced by the carrier-forcing block), so a `(:: ... :float)`
read MUST still reinterpret. Fixed by gating the skip on the spec's actual
`result_type` (looked up by clone name), mirroring the first loop's `type_eq`
guard. This was the source of the `0.5 -> 4.60268e+18` garbage the report's
experiment hit.

Step 3: the `TUR_SLOT_IS_COLL` resolved-type fallback in `emit_module.c` is
removed -- the declared-type path alone is now sufficient.

Validation: `tce3-map-cstr-val` prints `0.5`; `bash tests/run.sh` green
(1826 passed, 0 failed) after regenerating the `conv-byval-adt-nested-inline`,
`map-typed-consumer`, and `mutmap-typed-consumer` snapshots; `bash
tests/run-turi.sh` unchanged from baseline (1313/33/442).

---

## One-line summary

For a `defn`/`fn` parameter or return annotated with a **multi-type-param**
struct application -- `(Map K V)`, `(MutableMap K V)` -- the `Type` stored in
`fd->param_types[i]` / `fd->return_type` is a **degenerate TY_APP**: a `kind ==
TY_APP` shell whose `as.app.fn` / `as.app.arg` are NULL. Walking the `app.fn`
chain to recover the head StructDef hits NULL before reaching a `TY_STRUCT`, so
`type_extract_struct_app` returns false, and every predicate built on it
(`type_is_heap_struct`, `type_is_heap_vec`, ...) reports false for a type that
is really a heap collection. Single-type-param `(Vec A)` is unaffected.

## Root cause (file:line)

`src/compiler/elab_fns.c`, the FnDef param-type realiser
(`fd->param_types[i] = ...`, currently lines ~3413-3446):

```c
fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
for (uint8_t i = 0; i < n_params; i++) {
    if (param_kinds[i] == TY_FN && params[i]->type.kind == TY_FN) {
        fd->param_types[i] = params[i]->type;            // preserve fn sig
    } else if (param_kinds[i] == TY_STRUCT && params[i]->type.kind == TY_STRUCT) {
        fd->param_types[i] = params[i]->type;            // preserve StructDef
    } else if (/* TY_REF_IMMUT / TY_REF_MUT */) {
        fd->param_types[i] = params[i]->type;            // preserve borrow + lifetime
    } else if (/* TY_PTR_VOID with inner */) {
        fd->param_types[i] = params[i]->type;            // preserve ptr<T> pointee
    } else if (/* TY_INT param refined to TY_ADT */) {
        fd->param_types[i] = params[i]->type;            // preserve refined ADT
    } else {
        fd->param_types[i] = type_from_kind(param_kinds[i]);   // <-- spine lost here
    }
}
```

There is a preserve case for `TY_FN`, `TY_STRUCT`, borrows, `:ptr<T>`, and a
match-refined ADT -- but **none for `TY_APP`**. A `(Map K V)` parameter has
`param_kinds[i] == TY_APP`, so it falls to the `else` and is rebuilt with
`type_from_kind(TY_APP)`. That helper (`src/compiler/types.c:283`) is just:

```c
static Type type_from_kind(TypeKind k) {
    Type t;
    memset(&t, 0, sizeof(t));   // <-- as.app.fn = as.app.arg = NULL
    t.kind = k;
    ...
}
```

So the result is a `TY_APP` with NULL spine. The **properly-spined** type is
sitting right there in `params[i]->type` -- `fn_type_from_form_impl`
(`elab_fns.c:199-233`) builds it correctly via `type_app(...)`, which retains
the StructDef-headed `app.fn` chain (`types.c:2843-2846`). It is discarded by
the realiser's fallback.

(The MutableMap archive note
`docs/archive/history/mutmap-multi-param-producer-typing-blocked.md` already
observed the symptom in passing -- "`fd->return_type` for a `(MutableMap K V)`
return is a degenerate `TY_APP` (`type_from_kind(TY_APP)`), no spine/def" -- but
attributed it to the return path and never named the realiser fallback as the
shared source for *both* params and return.)

## Why `(Vec A)` escapes but `(Map K V)` does not

Empirically the single-type-param Vec accessors keep a usable declared type
(the carrier-forcing block has keyed on `type_is_heap_vec(fd->param_types[i])`
for Vec since the Vec slice and it fires correctly), whereas the two-param Map /
MutableMap accessors do not. The realiser fallback is the same line for both, so
the divergence is upstream in how `param_kinds[i]` / `params[i]->type` are
realised for a one-arg vs multi-arg application -- worth pinning precisely as
part of any fix, since "make multi-param look like single-param here" is the
shape of the fix.

## Minimal repro / observation

At `emit_abi_register_call` (`src/compiler/emit_module.c`), for the
`map-assoc-eq-o` accessor whose first param is `(Map K V)`:

```
fd->param_types[0].kind            == TY_APP   (21)
walk app.fn chain                  -> NULL before any TY_STRUCT
type_extract_struct_app(&p0,...)   == false
type_is_heap_struct(p0)            == false
type_is_heap_vec(p0)               == false
```

while the *resolved* call-site arg type extracts fine (`Map__int__float`). The
declared/resolved split is the whole bug.

## Impact (where the fallback already had to be paid)

- **`src/compiler/emit_module.c`, float/cstr carrier-forcing block.** Fixing
  `Eq[Map]` by-value typing
  (`docs/archive/eq-map-typed-consumer-blocked-on-transparent-newtype.md`) hit
  this directly: with the declared `(Map K V)` spineless, the block could not
  see that param 0 was the heap collection, so it never forced the `val :V`
  slot to the int64 carrier, monomorphizing it to `double` and truncating
  `0.5 -> 0`. The shipped fix works around it with a `TUR_SLOT_IS_COLL(decl,
  resolved)` macro that falls back to the **resolved** slot type when the
  declared type is a bare `TY_APP`.
- **Producer typing** (`map-new` / `mutmap-new`) only works because the
  inline-C body returns through `__TUR_RET__`, which routes interning down the
  `abi_changes` path instead of the `type_is_heap_vec(fd->return_type)`
  producer-result gate (which the degenerate return type silently fails).

Both are live workarounds; neither addresses the representation defect, so the
next multi-param `:heap` consumer will trip on it again.

## Fix directions

The obvious fix -- add a `TY_APP` preserve case to the realiser, mirroring the
`TY_STRUCT` one -- is **not** a clean drop-in:

```c
} else if (param_kinds[i] == TY_APP && params[i]->type.kind == TY_APP
           && params[i]->type.as.app.fn) {
    fd->param_types[i] = params[i]->type;   // preserve spined application
}
```

Verified experiment (this report): with that case added AND the
`emit_module.c` resolved-fallback neutered, `tce3-map-cstr-val`'s float
round-trip prints `4.60268e+18` (garbage), not `0.5`. So preserving the spine
flips OTHER signature-lowering paths that currently assume the declared
multi-param collection type is spineless (e.g. a now-spined `(Map K V)` param
that downstream code lowers differently than the degenerate shell). A real fix
must:

1. Restore the spine at the realiser (the one-line preserve case above), AND
2. audit every reader of `fd->param_types` / `fd->return_type` that today
   tolerates -- or depends on -- the degenerate shell, and make it handle the
   spined form (the carrier-forcing block, the producer-result gate, any
   `type_c_name` / signature emit that special-cases the shell), THEN
3. drop the resolved-type fallbacks (`TUR_SLOT_IS_COLL`'s second clause; the
   `__TUR_RET__`-only producer path can stay, it is orthogonal).

Because step 2 is a cross-cutting audit with snapshot blast radius, this is a
deliberate follow-up, not a drive-by. Until then the per-consumer resolved-type
fallback is the correct local pattern.

## How to validate a fix

- `fd->param_types[i]` for a `(Map K V)` / `(MutableMap K V)` param walks its
  `app.fn` chain to a `TY_STRUCT` head; `type_is_heap_vec` returns true on it.
- `tce3-map-cstr-val` (`Map int float` round-trip) prints `0.5` with the
  `emit_module.c` resolved-fallback **removed** -- proving the declared-type
  path alone is now sufficient.
- `bash tests/run.sh` green after the coordinated snapshot regen.
- `bash tests/run-turi.sh` interpreter-parity-neutral.

## Related

- `docs/archive/eq-map-typed-consumer-blocked-on-transparent-newtype.md` -- the
  Eq[Map] by-value fix that paid the resolved-type fallback.
- `docs/archive/history/mutmap-multi-param-producer-typing-blocked.md` -- first
  sighting of the degenerate `(MutableMap K V)` return type.
- `src/compiler/elab_fns.c` realiser loop -- the `else` fallback that drops the
  spine.
- `src/compiler/emit_module.c` `type_is_heap_vec` + the carrier-forcing block --
  the readers that carry the workaround.
