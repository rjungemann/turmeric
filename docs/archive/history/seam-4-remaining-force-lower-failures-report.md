# seam-4 (defstruct-as-defadt): remaining force-lower failures

> **RESOLVED.** All five documented roots now pass under the `TUR_FORCE_LOWER`
> probe (`--enable=defstruct-as-defadt`):
>
> 1. `defopaque-struct-payload-through-unsafe-lift` -- fixed (box the by-value
>    lowered-ADT arg to the int64 carrier at a carrier-ABI helper call site).
> 2. `fn-field-unboxed` -- fixed (read a lowered record-ADT `fn` field at its
>    int64 carrier width, not the fn's sub-word result width, before the call's
>    pointer re-specialisation).
> 3. `letrec-self-recursive-carrier-struct-return` -- fixed (heap-box a wide
>    by-value ADT arg into the closure thunk's `__tur_b4box_*` carrier param).
> 5. `typeclass-bounded-wrapper-heterogeneous-dispatch` -- fixed (spill an
>    instance method's by-value aggregate return into the bounded wrapper carrier
>    base's int64 return).
>
> 4. `result-over-struct-with-option-field-typedef-order` was already resolved by
>    the landed seam-4 PR (#570) before this pass; it passes as-is.
>
> All fixes live in `src/compiler/emit_expr.c`; the default by-value suite stays
> `1862 passed, 0 failed`. A probe-only force-lower sweep over the 182
> lowering-exercising fixtures went from 15 -> 11 failures with these changes (a
> strict subset, no new failures); the remaining 11 are pre-existing on the
> probe-only baseline and are NOT among these five (several are probe artifacts
> -- effect/capability `defdata` fixtures the real `--enable` gate never lowers).
> See `docs/archive/history/seam-4-remaining-force-lower-failures.md`.

**Severity:** medium (force-lower only; the default by-value path is unaffected
-- default suite is 1863/0). Each is exercised by running the named fixture
under the `TUR_FORCE_LOWER` probe in `defstruct_lowers_to_adt`
(`src/compiler/elab_structs.c`), the probe used for seam-4 development (never
committed).

As of this writing a full force-lower build+run sweep over the 182
lowering-exercising fixtures (those containing `defstruct`/`make-struct`,
excluding tsan/spices/dedicated-runner) is **PASS=175, and these 5 remaining
failures** (env-only `-lturi` link false-positives excluded). The clean,
self-contained seam-4 fixes have all landed; what remains are five distinct
deeper roots, each needing its own focused pass.

---

## 1. defopaque-struct-payload-through-unsafe-lift (BUILDFAIL)

**Error:** `incompatible type for argument 2 of '_un_unset_hyraw'`.

The specialized wrapper
`box_set___spec__..._tur_adt_Pos(int64_t b, tur_adt_Pos v)` passes its by-value
`tur_adt_Pos v` to the GENERIC `[A]` inline-C helper `__set-raw`, whose emitted
C signature keeps the carrier ABI: `_un_unset_hyraw(int64_t, int64_t)`.

**Root:** a by-value lowered-ADT argument crossing into a generic inline-C
helper's `int64` carrier parameter is not boxed at the call site. The construct
path boxes exactly this crossing (`emit_type_is_byvalue_adt` ->
malloc+copy+`(int64_t)(intptr_t)` in `emit_expr.c` ~3408), but a plain call to a
carrier-ABI helper does not.

**Fix direction:** either box the by-value ADT arg to the int64 carrier at a
call whose callee param is the int64 carrier (mirror the construct-field
boxing), or per-instantiation-monomorphize the generic inline-C helper to take
the concrete aggregate (the lowered analogue of the archived
`generic-inline-c-struct-arg-monomorphises-to-int64` fix).

## 2. fn-field-unboxed (RUNFAIL -- SIGSEGV)

A non-parametric `:copy` struct with a fully-typed fn-ptr field
`op : (fn [int32] int32)`. Expected `9` / `49`; under lowering the call through
`(.op cb 3i32)` segfaults.

**Root:** the struct path emits a typed function-pointer field
(`tur_fnptr_..._t`, Phase E) so the call needs no `intptr_t` cast. The lowered
record-ADT named layout does not carry the typed fn-ptr field the same way --
the field is read as the int64 carrier and called as a raw pointer (thin/fat or
cast mismatch), jumping wrong.

**Fix direction:** the lowered record-ADT named-layout typedef must carry a
typed fn-ptr field for a concrete `(fn ...)` field (mirror the struct Phase E
fn-ptr typedef in `emit_registered_struct_app_rec` / the named-layout ADT
emit), and `(.op cb x)` must dispatch through it.

## 3. letrec-self-recursive-carrier-struct-return (BUILDFAIL)

**Error:** `incompatible type for argument 3 of '__fn_1271'`.

A letrec-bound self-recursive `fn` returning a `:copy` `Box` (carrier-lowered).
The recursive self-call in the `if` arm mistypes its `Box` argument/return.

**Root:** the letrec analogue of #460's top-level-defn RR1 fix. `elab_letrec`'s
Pass-A placeholder collapses every non-scalar return to `TY_INT`, so under
lowering the self-call's `Box` arg/return comes back as the bare `int` carrier
instead of the `Box` ADT, desyncing the recursive call's signature.

**Fix direction:** carry the declared return/param FULL type (the `Box` ADT) on
the letrec Pass-A placeholder rather than collapsing to the int64 carrier --
the letrec counterpart of the RR1 block.

## 4. result-over-struct-with-option-field-typedef-order (BUILDFAIL)

**Error:** `unknown type name 'User'`.

Under lowering `tur_adt_Result__User__cstr` embeds `User` BY VALUE
(`struct { bool _0; User _1; const char * _2; } Result;`). `User` itself embeds
`Option__cstr`, so `User`'s typedef is pushed after `Option__cstr`; but the
`Result__User__cstr` app-monomorph typedef is emitted BEFORE `User`'s base
typedef.

**Root:** a topological-ordering problem ACROSS two emit passes -- the
non-parametric base-ADT typedefs and the parametric app-monomorph typedefs --
that must satisfy `Option__cstr < User < Result__User__cstr` while `User` lives
in one registry and the two `__app` monomorphs in another. At default `Result`
embedded `User *` (pointer), so the existing forward-`typedef struct User User;`
shim (`types.c:1416`) sufficed; a by-value embed needs `User`'s FULL typedef
ordered first.

**Fix direction:** `emit_registered_adt_app_rec`'s dependency pre-pass
(`types.c` ~1518) must, for a ctor field that resolves to a by-value base
ADT/struct, emit that base type's FULL typedef before its own (not just a
forward decl) -- unifying the base-ADT and app-monomorph emit ordering.

## 5. typeclass-bounded-wrapper-heterogeneous-dispatch (BUILDFAIL)

**Error:** `incompatible types when returning type 'tur_adt_Vel' but 'int64_t'`.

A function quantified over `[S] [(StorageOps S)]` with a struct-returning
wrapper, monomorphized at two carrier backends (`Dense`/`Sparse`).

**Root:** a bounded-instance wrapper spec's struct return crosses the
carrier/by-value boundary -- the spec body returns the by-value aggregate
(`tur_adt_Vel`) where the wrapper's int64 carrier return is declared (or the
reverse). The carrier<->by-value return-bridge reconciliation (the family
fixed for ordinary specs) does not fire for a bounded-instance wrapper spec.

**Fix direction:** extend the return-bridge reconciliation
(`fn_body_tail_byvalue_carrier_type` / the construct return spill, `emit_fns.c`)
to cover a bounded-`[(C S)]` wrapper spec's struct return.
