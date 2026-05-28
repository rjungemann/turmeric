# Aggregate Carrier ABI -- Call-Site Audit

> **Status:** Phase 1 complete
> **Produced by:** Phase 1 of `docs/aggregate-carrier-abi-plan.md`
> **Last Updated:** 2026-05-28

This checklist enumerates every compiler site that emits a value of aggregate
type (struct, ADT, Vec, Result, Option, Cons, Pair, Tuple2, etc.), classifying
each by the ABI it produces and the ABI its consumer expects.  Sites are marked
**[BUG]** where source and sink disagree without a bridge, **[OK]** where the
bridge already exists, and **[RISK]** where the bridge exists but is fragile or
incomplete.

Phases 2-5 of the plan will work through the **[BUG]** rows.

---

## Legend

| Tag | Meaning |
|-----|---------|
| `CK_CARRIER` | Value is an `int64_t` holding a heap pointer or inline 8-byte payload |
| `CK_CONCRETE` | Value is the concrete C struct/scalar type (by value or by pointer) |
| **[BUG]** | Source and sink disagree; no bridge emitted -- active KB cluster |
| **[OK]** | Bridge already in place and correct |
| **[RISK]** | Bridge exists but relies on a fragile condition or stale state |

---

## Site 1 -- `elab_types.c:1663-1676` -- `::` ascription (KB-004)

**File:** `src/compiler/elab_types.c` (`elab_ascribe`)
**Lines:** 1663-1676

```c
if (src_kind != dst_kind) {
    int src_size = type_size_bytes(src_kind);
    int dst_size = type_size_bytes(dst_kind);
    if (src_size > 0 && dst_size > 0 && src_size == dst_size) {
        /* insert EX_REINTERPRET */
    }
}
/* otherwise: plain EX_ASCRIBE -- no bridge */
```

**Source ABI:** `CK_CARRIER` (`TY_INT`, `int64_t` heap pointer from e.g. `result-map`)
**Sink ABI:** `CK_CONCRETE` (specialised struct, e.g. `Result__int__int`)
**Bridge present:** No -- the `type_size_bytes` guard only fires for same-size
scalar kinds.  A carrier `int64_t` has size 8; a multi-field struct has size
>8, so the guard fails silently and a plain `EX_ASCRIBE` node is emitted.
Downstream `ok-val` specialisation receives the `int64_t` where a
`Result__int__int` is expected, causing a C-level type error.
**Status:** **[BUG]** -- KB-004
**Fix target:** Phase 3 (`emit_carrier_bridge` in the ascription path)

---

## Site 2 -- `emit_expr.c:184` -- `let`-binding C-type declaration (KB-010)

**File:** `src/compiler/emit_expr.c` (`emit_let_value`)
**Line:** 184

```c
buf_printf(body, "%s %s = %s;\n", emit_type_c_name(ctx, b->type), bn, iv);
```

**Source ABI:** `CK_CARRIER` (`int64_t` -- e.g. return value of `vec_new()`,
`hamt_new()`, or any generic stdlib constructor)
**Sink ABI:** `CK_CONCRETE` (binding's annotated type, e.g. `Vec__int`)
**Bridge present:** No -- the emitter declares the variable with the
annotated concrete C type (`Vec__int v_532`) but assigns the carrier-typed
initialiser (`vec_new()` returns `int64_t`).  This produces a C type error
or silent misinterpretation.  The annotation is on the binding (`b->type`)
which is set by the type-checker to the concrete struct type, while the
init expression's type is `TY_INT` (carrier).
**Status:** **[BUG]** -- KB-010
**Fix target:** Phase 3 (`emit_carrier_bridge` in the let-binding path)

---

## Site 3 -- `elab_typeclasses.c` -- `definstance` body parameter convention (KB-012)

**File:** `src/compiler/elab_typeclasses.c` (`elab_definstance`, body
parameter type assignment)
**Relevant code:** parameter type assignment for parametric typeclass
instance bodies (no `CarrierKind` enum exists yet; the bug is that the
generated C function signature uses `int64_t` for the dispatching argument
while the body dereferences it as a concrete struct pointer)

**Source ABI:** `CK_CARRIER` (call site ships the dispatching value as an
`int64_t` heap pointer -- the carrier convention for all generic values)
**Sink ABI:** `CK_CONCRETE` (the instance body code casts `int64_t *` to
`tur_adt_Tuple2__int__int *` and reads fields directly)
**Bridge present:** No bridge at the function-signature level.  The
specialised instance body assumes concrete-by-value while the call site
passes the carrier.  Field-offset arithmetic on a non-dereferenced
`int64_t` is a SEGV.
**Status:** **[BUG]** -- KB-012
**Fix target:** Phase 4 (choose concrete-by-value as the canonical
convention; route call sites through `emit_carrier_bridge`)

---

## Site 4 -- `elab_call.c:1905-1928` -- generic result wrapping / stale cache (KB-015)

**File:** `src/compiler/elab_call.c` (`elab_call_binding`)
**Lines:** 1905-1928

```c
Type call_result_type = result_type;
bool wrap_generic_result = false;
if (fn_type.kind == TY_FN &&
    fn_type.as.fn.result_kind == TY_TYVAR) {
    call_result_type = TYPE_INT;
    wrap_generic_result = (result_type.kind != TY_INT);
}
// ...
if (wrap_generic_result) {
    return call_wrap_reinterpret(e, out, result_type.kind, call->span);
}
```

**Source ABI:** `CK_CARRIER` (`call_result_type = TYPE_INT` when the
function's return kind is `TY_TYVAR`)
**Sink ABI:** `CK_CONCRETE` (`result_type` derived from the binding's
`result_full_type`, which is shared across all call sites of the same
binding)
**Bridge present:** `EX_REINTERPRET` is inserted **[RISK]** -- but
`result_type` is derived from `fn_type.as.fn.result_full_type` (the
binding's full-type slot), which is a **shared mutable field** reused
across call sites.  When the same generic function (e.g. `thead`) is
called twice in the same scope with the same instantiation, the second
call reads a `result_type` that was already materialised by the first
call's specialisation pass, producing a stale carrier-typed slot.  The
`EX_REINTERPRET` wraps the already-concrete result in a second
reinterpret, corrupting the value.
**Status:** **[RISK]** → **[BUG]** on second call -- KB-015
**Fix target:** Phase 5 (rederive `result_type` from the call's resolved
instantiation each time; do not cache across call sites)

---

## Site 5 -- `emit_expr.c:686-719` -- `EX_REINTERPRET` emission

**File:** `src/compiler/emit_expr.c`
**Lines:** 686-719

```c
case EX_REINTERPRET: {
    // If inner call has a matching ABI specialization, skip the cast.
    for (uint32_t si = 0; ...) { ... if (match) return emit_value(...inner call...); }
    // Otherwise emit union bitwise reinterpret.
    buf_printf(&out, "((union { %s s; %s d; }){.s = %s}).d", ...);
}
```

**Source ABI:** `CK_CARRIER` or `CK_CONCRETE` scalar (union reinterpret
applies to equal-size scalars only)
**Sink ABI:** `CK_CONCRETE` scalar
**Bridge present:** Yes -- union cast **[OK]** for scalar-scalar crossings.
**Limitation:** The union cast is only valid when `sizeof(src) == sizeof(dst)`.
Carrier→multi-field-struct crossings cannot go through this path; those
are the KB-004/KB-010 bugs above.
**Status:** **[OK]** for scalar reinterprets; **not a path for struct crossings**

---

## Site 6 -- `emit_expr.c:1435-1468` -- monomorphic constructor argument unwrapping (TS4P2)

**File:** `src/compiler/emit_expr.c`
**Lines:** 1435-1468

```c
if (suffix && arg && arg->kind == EX_REINTERPRET &&
    arg->as.reinterpret_.target_kind == TY_INT &&
    arg->as.reinterpret_.expr) {
    arg = arg->as.reinterpret_.expr;  // unwrap carrier before passing to mono ctor
}
```

**Source ABI:** `CK_CARRIER` (argument was wrapped in `EX_REINTERPRET` to
box a scalar into `int64_t` for generic parameter passing)
**Sink ABI:** `CK_CONCRETE` (monomorphised constructor expects the concrete
scalar type, e.g. `float`)
**Bridge present:** Yes -- unwraps the reinterpret node **[OK]**
**Status:** **[OK]**

---

## Site 7 -- `emit_expr.c:720-732` -- `EX_UNION_INJECT` (IT4 tagging)

**File:** `src/compiler/emit_expr.c`
**Lines:** 720-732

```c
buf_printf(&out, "TUR_TAG(%lld, (int64_t)(intptr_t)(%s))", tag_idx, inner);
```

**Source ABI:** `CK_CONCRETE` aggregate (struct pointer or scalar)
**Sink ABI:** `CK_CARRIER` (packed into `tur_tagged_t` int64_t payload)
**Bridge present:** Yes -- explicit `(int64_t)(intptr_t)` cast **[OK]**
**Status:** **[OK]**

---

## Site 8 -- `emit_expr.c:744-754` -- `EX_ANY_CAST` (IT4 unboxing)

**File:** `src/compiler/emit_expr.c`
**Lines:** 744-754

```c
buf_printf(&out, "((%s)(intptr_t)TUR_UNTAG(%s))", type_c_name(target), inner);
```

**Source ABI:** `CK_CARRIER` (`TUR_UNTAG` extracts the `int64_t` payload)
**Sink ABI:** `CK_CONCRETE` (cast to target type)
**Bridge present:** Yes -- explicit `(intptr_t)` chain **[OK]**
**Status:** **[OK]** (caller responsible for tag correctness)

---

## Site 9 -- `emit_expr.c:3252-3253` -- ADT pattern match struct cast

**File:** `src/compiler/emit_expr.c`
**Lines:** 3252-3253

```c
buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
           adt_c_name, adt_c_name, scrut_val);
```

**Source ABI:** `CK_CARRIER` (`scrut_val` is the `int64_t` scrutinee)
**Sink ABI:** `CK_CONCRETE` (struct pointer for field extraction)
**Bridge present:** Yes -- explicit `(tur_adt_Name *)(intptr_t)` cast **[OK]**
**Status:** **[OK]**

---

## Site 10 -- `emit_expr.c:3061-3070` -- union match arm binding (IT4)

**File:** `src/compiler/emit_expr.c`
**Lines:** 3061-3070

```c
buf_printf(body, "%s %s = (%s)(intptr_t)TUR_UNTAG(%s);\n",
           ctype, bname, ctype, scrut_tmp);
```

**Source ABI:** `CK_CARRIER` (`tur_tagged_t` payload after `TUR_UNTAG`)
**Sink ABI:** `CK_CONCRETE` arm variable type
**Bridge present:** Yes -- explicit `(intptr_t)` chain **[OK]**
**Status:** **[OK]**

---

## Site 11 -- `emit_module.c:256-343` -- ABI specialisation registration

**File:** `src/compiler/emit_module.c` (`emit_abi_register_call`)
**Lines:** 256-343

Instantiates generic parameter/result types from `AbiTypeBinding`s,
detects C-level ABI changes, creates `EmitAbiSpecialization` records, and
deduplicates against existing specialisations.  Deferred clone codegen
fires at `emit_module.c:1145-1148`.

**Source ABI:** `CK_CARRIER` (generic `TY_TYVAR` parameters and result)
**Sink ABI:** `CK_CONCRETE` (instantiated concrete types)
**Bridge present:** Yes -- the specialisation system generates a concrete
clone function and routes calls to it via `matched_spec` in `emit_expr.c`
**[OK]**.  The `EX_REINTERPRET` scan at `emit_module.c:414-421` ensures
reinterpret-wrapped calls are registered with the correct result type
override.
**Status:** **[OK]**

---

## Summary of bug clusters

| # | File | Line(s) | KB | Status | Phase |
|---|------|---------|-----|--------|-------|
| 1 | `src/compiler/elab_types.c` | 1663-1676 | KB-004 | **[BUG]** | Phase 3 |
| 2 | `src/compiler/emit_expr.c` | 184 | KB-010 | **[BUG]** | Phase 3 |
| 3 | `src/compiler/elab_typeclasses.c` | definstance body params | KB-012 | **[BUG]** | Phase 4 |
| 4 | `src/compiler/elab_call.c` | 1905-1928 | KB-015 | **[RISK→BUG]** | Phase 5 |
| 5 | `src/compiler/emit_expr.c` | 686-719 | -- | **[OK]** (scalars only) | -- |
| 6 | `src/compiler/emit_expr.c` | 1435-1468 | -- | **[OK]** | -- |
| 7 | `src/compiler/emit_expr.c` | 720-732 | -- | **[OK]** | -- |
| 8 | `src/compiler/emit_expr.c` | 744-754 | -- | **[OK]** | -- |
| 9 | `src/compiler/emit_expr.c` | 3252-3253 | -- | **[OK]** | -- |
| 10 | `src/compiler/emit_expr.c` | 3061-3070 | -- | **[OK]** | -- |
| 11 | `src/compiler/emit_module.c` | 256-343 | -- | **[OK]** | -- |

**Open question (from plan):** Does `make-struct` always produce a carrier?
Answer from audit: No -- `EX_MAKE_STRUCT` at `emit_expr.c:2567-2593`
emits a C99 compound literal of the concrete struct type (`CK_CONCRETE`).
Only handle-returning functions like `vec_new()` and `hamt_new()` return
`CK_CARRIER`.
