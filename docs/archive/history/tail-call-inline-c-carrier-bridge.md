# Pure-Turmeric `defn` returning `(Result A B)` did not bridge when its body was a tail call to an inline-C carrier-ABI helper

**Status:** Resolved
**Severity:** Hard compile error. Codegen produced C that failed to compile
(`incompatible types when returning type 'int64_t' but 'Result_A_B' was expected`).
Blocked any thin wrapper around an inline-C helper returning a Result/Option.
**Discovered:** 2026-06-17, during turmeric-spices `tur-tourist` follow-up work.
**Resolved:** 2026-06-17, same session.

## Summary

A pure-Turmeric `defn` whose declared return type is a parametric/ADT aggregate
(e.g. `(Result cstr cstr)`, `(Option cstr)`) was lowered with the by-value
struct ABI, but when its body was a tail call to an *inline-C* helper that
returned the same aggregate (lowered with the carrier `int64_t` ABI), no
bridge was inserted at the return. The generated body was a bare
`return inlineC_helper(...);`, which failed to type-check.

The non-tail case (`(let [r (callee ...)] r)`) already worked: the carrier
let-binding + var-read path already had the bridge wired up. Only the tail
position was missing.

## Root cause

`fn_body_tail_is_carrier_producer` in `src/compiler/emit_fns.c` (line 209)
is the predicate the by-value/carrier bridge keys off. It only recognised
two kinds of tail call as carrier producers:

1. `b->is_construct_template` -- the `ok`/`err`/`some`/`none` builtins.
2. `__inst_*` -- typeclass instance methods.

It did not recognise the third source of `int64_t`-returning calls in this
codebase: an ordinary user `defn` whose body is `EX_INLINE_C` and whose
declared return uses the carrier ABI. The signature emitter (`emit_fns.c`,
around line 553-560) lowers exactly that shape to `int64_t`, so calls to
such helpers come back as the carrier handle -- but the predicate skipped
them, so the carrier->by-value deref at `emit_fns.c:1189` never fired.

## Fix

`src/compiler/emit_fns.c:224` -- extend `fn_body_tail_is_carrier_producer`'s
`EX_CALL` case to also return `true` when the callee is an inline-C-bodied
`defn` returning a carrier-ABI type:

```c
if (b->body_is_inline_c && type_uses_carrier_abi(e->type))
    return true;
```

The condition mirrors the signature emitter's "inline-C + carrier-ABI return
-> `int64_t`" decision exactly. With it, the M5-straddle bridge at
`emit_fns.c:1189-1207` and the control-form temp declaration at
`emit_expr.c:451` (`emit_control_result_temp_decl`) both fire for these
calls, just as they already did for `ok`/`some`/`__inst_*` producers --
covering tail position, `if`-branched tails, `do`-block tails, and `let`-body
tails uniformly.

## Validation

`tests/fixtures/tail-call-inline-c-carrier-bridge/` exercises five tail
shapes (plain, Option mirror, `if`, `do`, `let`) against inline-C
`(Result cstr cstr)` / `(Option cstr)` helpers and asserts the result
payloads round-trip end to end. Full `bash tests/run.sh` is green
(1672 passed, 0 failed).
