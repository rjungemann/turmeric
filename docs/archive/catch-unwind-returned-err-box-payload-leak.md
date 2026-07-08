---
status: resolved
severity: low
discovered: 2026-07-08
discovered-by: catch-unwind-return-bridge-residuals fix (Part B/C shallow free)
resolved: 2026-07-08
area: compiled backend / runtime (catch-unwind, carrier/by-value return bridge, box lifetime)
---

## Resolution (2026-07-08)

Fixed as the fix direction below prescribed, taking the caveat's guidance to
avoid the resolve-time side effect. New helper
`result_err_arm_is_freeable_scalar` (`emit_core.c`) reads the **already-resolved**
return type's app args purely and structurally via `type_extract_adt_app` -- no
`emit_resolve_type` / `type_adt_app_def` / `adt_field_type_for_app` call at the
return site -- and returns true for a `(Result A B)` whose err arm B is an inline
scalar. Both bridge free sites now emit the full `tur_result_box_free` (reclaims
the 32 B payload) when that holds, else keep `tur_result_box_free_shallow`:

- `emit_fns.c` native return bridge (gates on the resolved `sink_rt`).
- `emit_fns.c` stackless CPS `gs_catch_descend` (gates on `ret_aggr_ty`).

The minimal repro now reports `0 bytes in use at exit` under valgrind; the
pointer/cstr/aggregate err arm correctly stays on the shallow free (documented
residual, no dangling/double-free). The two `Result int int` fixtures
(`catch-unwind-branch-result-return`, `catch-unwind-byvalue-result-return`)
regenerated to the full free; full suite green. The side-effect-free probe did
NOT reproduce the caveat's rc-elision perturbation (only those two fixtures
moved).

# A caught **err** box returned by value leaks its 32 B panic payload

When a function returns a `catch-unwind` / `catch-panic-of` box as a by-value
`(Result ...)` and the caught result is the **err** (a panic was caught), the
return bridge frees only the 24 B box struct and leaves the 32 B
`tur_panic_payload` behind: a bounded per-return leak. This is the deliberate
residual left by the `catch-unwind-return-bridge-residuals` fix
(`docs/archive/catch-unwind-return-bridge-residuals.md`, Parts B/C), split out
here so it is not forgotten. Long-standing shape, uncommon (catching then
directly returning the raw Result), does not block v1.

## Minimal repro

```turmeric
(defn make [] : (Result int int)
  (catch-unwind (fn [] : int (panic-with 7))))
(defn main [] : int
  (let [r (make)] (if (err? r) 1 0)))
;; ==> 32 bytes definitely lost (the tur_panic_payload; the 24 B box IS freed)
```

```sh
TUR=./build/tur
$TUR build /tmp/resid.tur -o /tmp/resid
valgrind --leak-check=full /tmp/resid
# 32 bytes in 1 blocks are definitely lost
```

The **ok** path (`(catch-unwind (fn [] : int 5))`) is already fully clean: the
box has no payload, so the shallow free reclaims everything.

## Root cause

The return-bridge box free was intentionally made a *struct-only* free
(`tur_result_box_free_shallow`, `emit_module.c:6628`) rather than the full
`tur_result_box_free` (which additionally frees the payload,
`emit_module.c:6615`). The reason is aliasing: the carrier->concrete readback
writes the box's `err_val` slot -- for a caught box that slot IS the panic
payload pointer -- into the returned aggregate's err field. For a
pointer/cstr/aggregate err arm that field is a *live* pointer into the payload,
so freeing the payload would dangle it. The shallow free is the safe universal
choice; the payload is then owned by whoever reads the returned err aggregate.

Free sites (both use the shallow free unconditionally):

- `src/compiler/emit_fns.c:3478` -- native return bridge.
- `src/compiler/emit_fns.c:1510` -- stackless CPS `gs_catch_descend`.

For the common **scalar** err arm (`(Result int int)`) the payload is NOT
aliased (the aggregate's err field is a plain word, a reinterpreted pointer
never dereferenced), so a full free would be safe there and would reclaim the
32 B. The escape-side sibling of this exact scalar test already exists as
`err_val_result_is_freeable_scalar` (`emit_core.c`, used by Part A).

## Fix direction

Gate the free per-arm: emit `tur_result_box_free` (full, reclaims the payload)
when the resolved Result's err field type is an inline scalar, else keep
`tur_result_box_free_shallow`. The err-field type comes from the same
`(Result A B)` monomorph the bridge already resolves.

**Caveat / blocked-on (worth its own look):** an earlier attempt did exactly
this, computing the err-field kind via `type_adt_app_def` +
`adt_field_type_for_app` + `emit_resolve_type` at the return site. That probe
**perturbed unrelated codegen** -- an ADT-value drop stopped being emitted in a
sibling function, turning a previously-clean program into a 4 B `ctor_*()` leak
(observed on a `match`-over-`defdata`-returning-catch-unwind fixture). That
points to a latent side effect in one of `emit_resolve_type` /
`type_adt_app_def` / `adt_field_type_for_app` when called during return
emission (state mutation / monomorph registration that alters a later
rc-elision decision). A safe fix should extract the err-field kind **without**
those calls (e.g. read the elaborator's already-resolved return-type app args
structurally), or first root-cause and remove the resolve-time side effect.

## Related

The opaque panic **value** (`panic-with`/`panic` payload *contents*, e.g. a
heap `panic "..."` string) is never freed by `tur_result_box_free` either -- by
design, since the value is opaque and may be an inline scalar or borrowed
elsewhere (see the `tur_result_box_free` comment, `emit_module.c`). That is a
separate, pre-existing residual shared with the scope-exit free, not introduced
here.
