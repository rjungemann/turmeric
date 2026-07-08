---
status: open
severity: low
discovered: 2026-07-08
discovered-by: catch-unwind thunk/box-leak + by-value-return-bridge fixes
area: compiled backend / runtime (catch-unwind, carrier/by-value bridge, box lifetime)
---

# `catch-unwind` residuals: `err-val` box leak, returned-box leak, and unbridged if/match catch-box return

Three bounded residuals left behind by the two landed catch-unwind fixes
(`docs/archive/catch-unwind-thunk-closure-leak.md`,
`docs/archive/catch-unwind-byvalue-result-return-mismatch.md`). All are
long-standing, per-catch-site bounded, and uncommon; grouped here so they are not
forgotten. None block v1.

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
