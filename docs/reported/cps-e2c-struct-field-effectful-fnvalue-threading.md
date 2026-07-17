# E2c: an effectful fn-value stored in a STRUCT FIELD, called via `.field`, evicts (capability-field cluster)

**STATUS: prerequisite landed (suite green 2203/0); threading is follow-on.**
The effect-row resolution prerequisite (commit 4801664 -- resolve
ERK_UNRESOLVED CtorField effect rows in effect_check Step 0a2) is IN, together
with its root-cause companion (commit 13aa356 -- zero-init CtorField.effect_row
on the positional/GADT ctor path, which the new resolve reader exposed as a
latent uninitialized-pointer).  Together they clear the spurious TUR-W0033 on
this cluster and let both effect_check and the CPS coloring see the field's
effect, with the full suite green.  The actual DK threading of the `.field`
call is the remaining work pinned here.

**Severity:** low-medium (correct on the fiber; endgame migration target).
Cluster (measured, all SIG-TAINT + eff=1 under `--enable=cps-tramp-resume`,
correct output): `effect-struct-field-row`, `capability-effect-poly`,
`effect-type-alias`.  (`fh-discharge-row` is a DIFFERENT residual -- effect-row
discharge via nested with-handler, not a field call.)

## The shape (pinned)

```turmeric
(defeffect Emit [s :cstr] :nil)
(defstruct Emitter :copy [run : fn #fx{Emit}])      ; effectful capability field
(defn main [] : int
  (let [em (make-struct Emitter (fn [s] (perform (Emit s))))]   ; effectful lambda in field
    (handle
      (do (.run em "struct field row") 0)            ; field-accessor call
      (Emit [s] k) (do (println s) (resume k nil)))))
```

`capability-effect-poly` is the same shape with a NAMED fn (`do-write-line`)
stored in the field instead of a lambda -- so the residual is the field CALL,
not the lambda-ness of the value.

## Root cause (two coupled gaps, both pinned in the emitted C)

1. **The field-stored lambda is never CPS-emitted / registered.**  Emitted C:
   `static void __fn_1282(int64_t s)` -- a plain direct fn, NO `__fn_1282__cps`,
   NO `__tur_cps_register`.  It evicts to fiber (SIG-TAINT), so the registry has
   no entry for it.  (Contrast E2a, where a lambda passed as a call ARG gets a
   `__cps` + a `__tur_e2reg_*` constructor.)

2. **The `.field` call is emitted as a raw indirect call, not a threaded one.**
   Emitted C for `(.run em "..")`:
   ```c
   (((int64_t (*)(const char *))(intptr_t)((int64_t)(__henv_156->em).run))("struct field row"))
   ```
   It loads the field fn-ptr, casts, and calls it DIRECTLY -- no
   `__tur_cps_lookup`, no `__kont`.  So the lambda's `perform (Emit s)` runs on
   the fiber with main's handler on the DK -> escape -> the taint model evicts
   the whole cluster to keep it consistent.

The admission gate (src/passes/cps_ir.c:2939-2957) only threads an effectful
fn-value call when the callee is a THREAD-PARAM binding
(`pf = e->as.call_.fn_binding; if (pf && cps_ir_thread_param_has(pf) ...)`).
A `.field` call has `fn_expr == EX_GET_FIELD` and NO `fn_binding`, so it falls
straight through to `CT_UNSUPPORTED` ("effectful fn-value call (E2 pending)").

## Fix direction (E2c -- three coordinated parts)

1. **Color + register the field-stored effectful fn-value.**  Extend the
   force-coloring (cps.c `cps_force_color_eff_fnval_args`, added for the E2
   pure-lambda subtyping slice) to also color a lambda / named-fn that flows
   into an effectful `fn #fx{E}` struct FIELD via `make-struct` (the field's
   resolved `CtorField.effect_row` is now available post-4801664).  Then the
   E2 registration loop (emit_cps_ir.c ~3604) must register it so
   `__tur_cps_lookup(field_fnptr)` resolves.

2. **Represent a field-load callee in CT_TAILCALL.**  Today
   `t->as.tailcall.fn` is a `const Binding *` (a param).  A field call's callee
   is an EXPRESSION (`EX_GET_FIELD`).  Either (a) bind the field load to a temp
   cvar first (letval) and let the via_registry path key off that CVar/atom, or
   (b) add a callee-atom variant to CT_TAILCALL.  Route (a) is smaller and
   reuses the existing `(intptr_t)<key>` lookup emission.

3. **Emit the registry-threaded field call.**  Mirror emit_cps_ir.c:4349's
   via_registry emission:
   `((int64_t (*)(<argc x int64_t,> DK *))__tur_cps_lookup((intptr_t)<field-fnptr>))(args, __kont)`.

## Verification recipe

- The 3 fixtures emit their `.field`-call site as a `__tur_cps_lookup(...)(...,
  __kont)` with zero `eff=1` and unchanged output.
- Flag-off byte-identical (all admission flag-gated on `g_opt_cps_tramp_resume`).
- FULL flag-on soundness sweep (every effect fixture flag-on == flag-off
  baseline) -- MANDATORY; this family is where the earlier blanket E2 attempt
  was reverted for unsoundness.

## Context

Stage E follow-on after the E2 row-poly cluster
(docs/archive/cps-e2-rowpoly-fnvalue-threading-boundary.md) and the E2
pure-lambda subtyping slice
(docs/reported/cps-e2-pure-lambda-into-effectful-fnvalue-param.md).  E2a threads
a fn-value PARAM; E2c threads a fn-value STRUCT FIELD.  Same registry channel
(`__tur_cps_lookup`), different callee provenance.
