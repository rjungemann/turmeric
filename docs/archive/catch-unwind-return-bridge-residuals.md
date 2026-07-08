---
status: resolved
severity: low
discovered: 2026-07-08
discovered-by: catch-unwind thunk/box-leak + by-value-return-bridge fixes
resolved: 2026-07-08
area: compiled backend / runtime (catch-unwind, carrier/by-value bridge, box lifetime)
---

# `catch-unwind` residuals: `err-val` box leak, returned-box leak, and unbridged if/match catch-box return

Three bounded residuals left behind by the two landed catch-unwind fixes
(`docs/archive/catch-unwind-thunk-closure-leak.md`,
`docs/archive/catch-unwind-byvalue-result-return-mismatch.md`). All are
long-standing, per-catch-site bounded, and uncommon; grouped here so they are not
forgotten. None block v1.

## Resolution (2026-07-08)

All three parts are fixed. Verified with valgrind on each repro (no leaks / no
invalid frees) and `bash tests/run.sh`: **1978 passed, 0 failed** (snapshots
regenerated for the new `tur_result_box_free_shallow` preamble helper and the
catch-box return frees). New regression fixture
`tests/fixtures/catch-unwind-branch-result-return` exercises the if-tail
(stackless) and match-tail (native) return bridges on the ok and caught paths.

### A -- `err-val`-inspected box now freed for a scalar err arm

The scoped-free escape walker (`binding_escapes_impl`, emit_core.c) now
whitelists `err-val` as a non-escaping accessor **when its result type is a
scalar** (`err_val_result_is_freeable_scalar` -- int/int64/float/bool family).
A caught box's `err_val` slot IS the panic-payload pointer; when the declared
err arm is a scalar the extracted value is a plain word (a reinterpreted
pointer bit pattern), never a live pointer INTO the payload, so
`tur_result_box_free` reclaiming the box+payload at scope exit cannot dangle
it. A pointer/cstr/aggregate err arm stays excluded (it would alias the
payload). The repro's 56 B (24 box + 32 payload) is now reclaimed.

### B -- returned caught box freed after the return bridge (sole-owned)

After the carrier->concrete return bridge materializes the aggregate, the
source box is freed (`tur_result_box_free_shallow`, a struct-only free that
never touches the payload -- the returned err aggregate may alias it). Gated on
`catch_box_tail_sole_owned`: a DIRECT catch-unwind tail is a fresh anonymous
box (always sole-owned); a let-bound VAR tail is freed only when it escapes
nowhere but the return
(`catch_box_binding_escapes_except` ignoring the return-tail use); an if/match
tail requires every arm to be sole-owned. This proves no double-free / no
dangling alias. The repro's 24 B box is reclaimed.

The escape walker gained `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` and the
fn->fat / poly coercion-wrapper cases (`EX_FN_TO_FAT`, `EX_POLY_TO_FAT`,
`EX_POLY_WRAP`) -- scoped to the catch-box variant so the fat-closure-env
analysis is byte-identical -- so the analysis sees through a catch box's own
`(catch-unwind ...)` initializer (whose thunk reaches it wrapped in
`EX_FN_TO_FAT`) instead of conservatively defaulting to escape.

### C -- if/match catch-box tail now bridged (both lowering paths)

Empirically the compile error surfaces on TWO paths, not one:

- **if-tail** lowers through the **stackless CPS machine** (`gs_catch_descend`):
  the caught box was delivered to a `ret_aggr` GSK_RETURN sink, which heap-boxed
  it as if it were the aggregate (`Result__int__int __rv = (__box)`, an invalid
  int64->struct initializer). Fixed by bridging the carrier box to the concrete
  aggregate (`gs_sink_return_aggr_type` + `emit_carrier_bridge`) before delivery.
- **match-tail** lowers through the **native** return path: the report's
  original int64->struct miscompile. `fn_return_needs_carrier_result_bridge` now
  sees through `if`/`match` whose arms are all catch boxes
  (`expr_tail_is_catch_box`), so the existing carrier->concrete return bridge
  fires on the merge temp.

Both paths also apply the Part B sole-owned box free.

### Remaining (smaller, deliberate)

A caught **err** box returned by value keeps the shallow (struct-only) free, so
its 32 B panic payload is left to the returned aggregate as its new owner
rather than risk dangling a pointer-typed err field; for a scalar err arm this
is a bounded per-return payload residual (the box's 24 B is always reclaimed).
An earlier scalar-gated full free that would have reclaimed it was reverted --
the `adt_field_type_for_app`/`emit_resolve_type` probe it needed perturbed
unrelated codegen (an ADT-value drop), which is not worth the 32 B. The opaque
panic *value* (`panic-with`/`panic` payload contents) is likewise never freed by
`tur_result_box_free`, matching the pre-existing scope-exit-free behavior.

## A. `err-val`-inspected let-bound box is not freed (56 B / catch)

The let-bound scoped free
(`catch_box_binding_escapes`, `emit_expr.c` `let_binding_box_freeable`)
whitelists only the read-only accessors `ok?` / `err?` / `ok-val`. `err-val` is
deliberately excluded: it hands back the box's panic-payload pointer, which
`tur_result_box_free` reclaims, so freeing the box at scope exit could dangle an
extracted payload. A caught Result inspected with `err-val` therefore still
leaks its 24 B `tur_result_box_t` + 32 B `tur_panic_payload`.

```turmeric
(defn main [] : int
  (let [r (catch-unwind (fn [] : int (panic-with 7)))]
    (if (err? r) (err-val r) 0)))
;; ==> 56 (24 direct, 32 indirect) bytes definitely lost
```

**Fix direction:** free the box at scope exit only when the `err-val` result
provably does not escape the scope (a second last-use check on the extracted
payload), or copy the payload out and free the box when the extraction is itself
scope-local.

## B. A returned caught box is not freed after the by-value return bridge (24 B / call)

When a function returns a `catch-unwind` box as a by-value `(Result ...)`, the
new return bridge (`emit_fns.c` `fn_return_needs_carrier_result_bridge`) copies
the box fields into the aggregate but does not free the heap box afterward.

```turmeric
(defn make [] : (Result int int)
  (let [r (catch-unwind (fn [] : int 5))]
    r))
(defn main [] : int
  (let [r (make)] (if (ok? r) (ok-val r) 0)))
;; ==> 24 bytes definitely lost (the tur_result_box_t copied out in make)
```

**Fix direction:** after the carrier->concrete bridge materializes the aggregate,
`tur_result_box_free` the source box -- but only when it is provably the sole
owner (the tail is a fresh catch-unwind box, as the bridge already requires), so
a shared/aliased carrier is never double-freed.

## C. An `if` / `match` tail whose arms are catch boxes is not bridged (cc error)

The return bridge's `fn_body_linear_tail` walk stops at a branch, so a function
whose by-value `(Result ...)` result comes from an `if`/`match` over
`catch-unwind` arms still hits the original int64->struct miscompile.

```turmeric
(defn make [cond : int] : (Result int int)
  (if (= cond 0)
    (catch-unwind (fn [] : int 5))
    (catch-unwind (fn [] : int 7))))
;; cc: incompatible types when returning 'int64_t' but 'tur_adt_Result__int__int'
```

This one is a compile error (an expressiveness hole), not a leak -- slightly
sharper than A/B but still uncommon (branching directly on which catch boundary
to run and returning the raw Result).

**Fix direction:** extend the catch-box-tail detection into `if`/`match` arms
(bridge when every arm is a catch-box producer), or bridge per-arm in
`emit_if_value` the way the by-value carrier-producer arms already are.

## Related (already tracked elsewhere)

The stackless-catch-unwind lowering keeps its own per-iteration aggregate-box
leak (a tail-recursive catch loop leaks 24 B/iter); that path is separate from
the native fixes above and was scoped under the now-archived
`docs/archive/catch-unwind-aggregate-followups-plan.md` (Part B).
