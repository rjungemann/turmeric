# Fix paper trail -- catch-unwind by-value Result return mismatch

Resolved 2026-07-08. Report:
`docs/archive/catch-unwind-byvalue-result-return-mismatch.md`.

## Symptom

```turmeric
(defn make [] : (Result int int)
  (let [r (catch-unwind (fn [] : int 5))]
    r))
```

`cc` error in `make`:
`incompatible types when returning type 'int64_t' but 'tur_adt_Result__int__int'
was expected`.

## Root cause

`tur_catch_unwind_box` yields a heap `tur_result_box_t *` carried as `int64_t`.
The declared return `(Result int int)` is a *distinct* monomorph emitted BY VALUE
(`tur_adt_Result__int__int`).  The let-result temp holds the int64 box; the
function `return`s it into a by-value struct slot with no carrier->concrete
bridge (the existing M4c/M5 return bridges only fire for `ok`/`err`/`some`/`none`/
`__inst_` carrier-producer tails, which a bare-variable catch box is not).

## Fix (`src/compiler/emit_fns.c`)

- `fn_body_linear_tail(e)` -- descend `let`/`do`/ascription to the tail leaf.
- `fn_body_binding_init(e, b)` -- find binding `b`'s initializer in the enclosing
  let chain.
- `fn_return_needs_carrier_result_bridge(ctx, fd, ret_ctype, ret_is_int64_carrier)`
  -- true when the return C type is a by-value aggregate (not int64, no `*`, not
  void) AND the linear tail is `EX_CATCH_UNWIND`/`EX_CATCH_PANIC_OF`, or an
  `EX_VAR` whose binding initializer is one.
- New return-emission branch (before the final `else`): when the predicate holds,
  emit `emit_carrier_bridge(strdup(ret_val), CK_CARRIER, CK_CONCRETE, sink_rt)`
  with `sink_rt` = the DECLARED return type (`*e->type.as.fn.result_full_type`
  resolved), whose `emit_type_c_name` is the by-value struct.  The bridge's
  canonical Result/Option readback casts the box to `tur_result_box_t *` and
  reconstructs the aggregate field-by-field (correct for sub-word payloads; a raw
  `*(T*)` deref would misread them).

### False start (why the gate is structural, not representational)

The first cut gated on `emit_type_c_name(resolve(body type)) == "int64_t" &&
!fn_body_tail_emits_byvalue_carrier_abi(...)`.  It mis-fired on
`ok__spec`/`err__spec`, whose `#{Construct}` (`ctor_Result__int__int(...)`) body
type ALSO collapses to `int64_t` under `emit_type_c_name` yet EMITS the by-value
aggregate.  Bridging it produced
`tur_result_box_t *__t = (tur_result_box_t *)(intptr_t)(<aggregate>)` -> `cc`:
`aggregate value used where an integer was expected`.  `emit_type_c_name` is not
a reliable "emitted as carrier" signal.  Keying on the catch-box tail
structurally is exact: only `tur_catch_unwind_box` results reach the branch, and
those are always the int64 heap box.

## Verification

- Repro + let/direct/caught shapes compile and run; `ok?`/`err?` observe the
  right tag; no use-after-free under valgrind.
- `ok`/`err` constructor specs unchanged (`return __ps_N;`, no box cast).
- Regression fixture `tests/fixtures/catch-unwind-byvalue-result-return`.
- `bash tests/run.sh`: 1976 passed, 0 failed.

## Deliberately left

- The copied-out box is not freed after the bridge (bounded per-return leak; a
  returned box has no single provable owner, and freeing an arbitrary carrier
  Result there risks a double free).
- An `if`/`match` tail whose arms are catch boxes is not bridged (the linear-tail
  walk stops at a branch).  Both uncommon.
