# Fix: pure fn in an effectful-typed capability field CPS-admits

Resolves `docs/archive/cps-effect-subtype-capability-pure-fn-in-effectful-field.md`.

## Summary

A pure fn (`pure-greet`, inferred row `#fx{}`) stored in a capability field
declared `[run : fn #fx{Write}]` and called through the field (`(.run act ...)`)
force-evicted the enclosing `main` to the fiber under
`--enable=cps-tramp-resume`, even though the compiler already proves the call
performs nothing (it raises TUR-W0033 for the unreachable Write clause).

## Root cause (measured on this branch)

The handled body lowers to a delegated `CT_LETRAW` call `(.run act ...)` then a
`KK_PROMPT` deliver.  `(.run act ...)` is an INDIRECT call (a fn-value field), so
`expr_collect_effects_acc`'s `EX_CALL` arm set `callee_overflow` -- the blunt
"reaches every colored peer" over-approximation.  `letraw_effect_free` treats
`ov` as effectful, returned false, so `handle_delim_ok` rejected the `CT_LETRAW`
and `main` evicted `BODY-STRUCT-OR-TAINT`.

Probed facts that redirected the fix from the ticket's hypothesis:
- The field access's declared type row (`fn_expr->type.as.fn.effect_row`) is
  **NULL** here -- the existing fn-value declared-row crediting block was not the
  culprit.
- The `CtorField.effect_row` for `[run : fn #fx{Write}]` is **`ERK_UNRESOLVED`**
  (the raw `#{Write}`, never lowered to `ERK_CONCRETE`), which both
  `effect_check` and the CPS classifier treat as empty -- hence W0033 fires for
  the pure AND the effectful case.
So the real force-evict was the `callee_overflow`, the ticket's flagged
secondary suspect.

## Fix (Approach A)

`src/compiler/emit_cps_ir.c`, `expr_collect_effects_acc` `EX_CALL`: an indirect
call whose `fn_expr` is a lowered `.field` capability access (`EX_GET_FIELD` with
an `adt_ctor`) is credited with the field's PRECISE `CtorField.effect_row`
(concrete effects marked as performs) -- mirroring `effect_check`'s
`collect_effects_in_expr` field path -- instead of setting `callee_overflow`.
For the pure fixture the field row is `ERK_UNRESOLVED` -> nothing credited ->
`letraw_effect_free` sees an effect-free body -> `main` admits and emits
`main__cps`.  Gated on `g_opt_cps_tramp_resume`; flag-off keeps the unconditional
overflow, so the shipping classifier is byte-identical.

## Why the effectful guardrail still holds

A genuinely Write-performing value stored in the field (`eff-greet`) is
address-taken (used as a value) -> a permanent fiber source -> `Write`
base-taints -> `main` (which handles Write) co-evicts `SIG-TAINT`.  This is
orthogonal to the local field crediting, so suppressing the overflow never
admits a `main` whose field call actually performs a handled effect on the
fiber.

## Verification

- Pure fixture: admitted (`main__cps`, zero `eff=1` evictions), prints
  `hello from pure`.
- Effectful companion: still evicts `SIG-TAINT`, runs on the fiber, prints
  correctly.
- New flag-on regression fixtures:
  `tests/fixtures/cps-tramp-resume-effect-subtype-capability` (positive, DK path)
  and `...-effectful` (fiber guardrail).
- `handle-effectful-fn-param-same-fn` and `cps-backend-fn-param-effectful`
  unchanged (fn-param indirect calls still overflow -- only the `EX_GET_FIELD`
  capability subcase is intercepted).
- `bash tests/run.sh`: 2196 passed, 0 failed; flag-gated, so flag-off is
  byte-identical (no snapshot churn).
