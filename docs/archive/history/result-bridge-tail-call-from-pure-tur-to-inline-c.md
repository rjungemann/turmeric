# Pure-Turmeric `defn` returning `(Result A B)` does not bridge when its body is a tail call to an inline-C carrier-ABI helper

**Status:** Resolved.
**Severity:** Was a hard compile error.  Codegen produced C that failed to
compile (`incompatible types when returning type 'int64_t' but
'Result_A_B' was expected`).  Blocked any thin wrapper around an inline-C
helper that returns a Result/Option.
**Discovered:** 2026-06-17, during turmeric-spices `tur-tourist` follow-up
work (Track C, retyping `param` / `capture` ctx parameter to `Ctx`).
**Resolved:** 2026-06-17, branch `claude/happy-knuth-9u41uw`.

---

## Summary

A pure-Turmeric `defn` whose declared return type was an algebraic data type
(here `(Result cstr cstr)`) was lowered with the by-value struct ABI (the
function's C signature was `Result__cstr__cstr (...)`).  When its body was
a tail call to an *inline-C* helper that returned the same ADT (lowered
with the carrier `int64_t` ABI), no bridge was inserted at the return: the
generated C body was simply `return inlineC_helper(...);`, which failed to
type-check because the helper returned `int64_t` and the enclosing function
returned a struct.

## Root cause

`fn_body_tail_is_carrier_producer` in
[`src/compiler/emit_fns.c`](../../src/compiler/emit_fns.c) is the predicate
that the M5 straddle case at `emit_fn_def` (lines 1189-1206) consults to
decide whether the by-value return needs the carrier->concrete bridge.  It
recognized #{Construct} producers (`ok` / `err` / `some` / `none`) and
typeclass-method impls (`__inst_*`) as carrier producers, but it did NOT
recognize a direct call into an inline-C-bodied function whose declared
return type uses the carrier ABI -- even though such a callee returns
`int64_t` at the C level for exactly the same reason.

The result: at the tail call site, the M5 straddle branch was skipped and
the fallback `buf_printf(file, "return %s;\n", ret_val);` emitted a bare
`return inlineC_helper(...);`, leaving the carrier handle unbridged.

## Fix

Extend the `EX_CALL` arm of `fn_body_tail_is_carrier_producer` with a
fourth recognizer: a call whose binding is inline-C-bodied AND whose
declared return type uses the carrier ABI.  Such a call's C return type is
`int64_t` (the carrier handle), so it must go through the same bridge as
the other carrier producers.

```c
if (b->body_is_inline_c && b->type.kind == TY_FN &&
    b->type.as.fn.result_full_type &&
    type_uses_carrier_abi(*b->type.as.fn.result_full_type))
    return true;
```

No new bridge-emission code was needed -- the M5 straddle case already
threaded the unbox via `emit_carrier_bridge(... CK_CARRIER, CK_CONCRETE
...)`; this fix just makes the predicate recognize the tail leaf so the
existing bridge fires.

## Validation

- `tests/fixtures/result-bridge-tail-call-to-inline-c/` -- a new compiled
  fixture: a pure-Turmeric `defn` returning `(Result cstr cstr)` whose body
  is a tail call to an inline-C `defn` returning the same type.  Two
  variants exercise the ok-side and err-side carriers.  Before the fix the
  fixture failed at the C compile step; after the fix it builds and prints
  `hello\nmissing`.
- The full compiled-fixture suite (`bash tests/run.sh`) passes: 1672
  passed, 0 failed (1671 baseline + 1 new regression fixture).

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the Track C
  audit; the discovered breakage was on the very thin wrappers the audit
  recommends keeping pure-Turmeric.  Fixing this bug unblocks landing the
  `param` / `capture` ctx-retype as a pure spice change.
- PR #402 (`Fix carrier-ABI bridge generation for control-form and
  by-value Option returns`) -- the previous bridge-generation fix on the
  same M5 straddle path; this fix extends that work to cover the inline-C
  tail callee case.
