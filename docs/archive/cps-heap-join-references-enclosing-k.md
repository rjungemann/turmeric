# CPS lifted heap-join helper references the enclosing continuation `k` (contract-nested) -- RESOLVED

**Severity:** medium (build failure on a nested-contract program under the CPS
backend; the direct emitter compiles it).  Surfaced by the `cps-backend`
graduation.

**Status: RESOLVED** -- `needs_heap_join` now rejects a heap-join whose join
body itself contains a cps->cps tail call (`jbody_has_cps_tailcall`,
src/compiler/emit_cps_ir.c), so such a function evicts to the direct emitter
instead of emitting a frame fn that references an out-of-scope `k`.
`contract-nested` is green; suite 2142/2142.

## Summary

A lifted heap-join frame helper emitted by the CPS backend can reference the
enclosing function's continuation parameter `k`, which the helper does not
receive -- an undeclared C identifier.

## Repro

```sh
./build/tur build tests/fixtures/contract-nested/input.tur -o /tmp/x
```

```
error: 'k' undeclared (first use in this function)
```

Emitted C:

```c
static intptr_t apply_hytwice_j0(intptr_t env, intptr_t t0__slot) {
    (void)env;
    int64_t t0 = (int64_t)(t0__slot);
    return inner__cps(t0, k);   /* cps->cps -- but `k` is not in scope here */
}
static int64_t apply_hytwice__cps(int64_t x, DK *k) { ... }
```

The join body is a non-tail `cps->cps` call that threads the *enclosing*
continuation `k`.  `letcont_is_heap_join` lifts the join into the frame helper
`apply_hytwice_j0`, but the helper's signature is `(env, value)` -- it has no
access to `k`, so `inner__cps(t0, k)` names an undeclared variable.

## Where to look

`src/compiler/emit_cps_ir.c`: `needs_heap_join` / `letcont_is_heap_join` and
`emit_heap_join`.  `needs_heap_join` rejects a join body that captures the join
*param* (`has_capture(jbody, param.id)`), but not one that references the
enclosing continuation `k`.  Either the heap-join lift must carry `k` into the
frame env (so the helper can thread it), or a join body that references `k` must
be rejected by `needs_heap_join` so the function evicts to the direct emitter.

## Resolution

Took the "reject in `needs_heap_join`" option.  The precise trigger was narrower
than "references `k`": a heap-join jbody that is *itself* a cps->cps tail call
(here `inner__cps(t0, k)`, KK_RET to a colored callee).  The lifted frame fn is a
value-transform `(env, value)` with NO continuation in scope, so it cannot thread
any continuation into a nested colored call.  `jbody_has_cps_tailcall` walks the
jbody's tail spine (`if`/`let` chains to their tail positions) and, if it finds a
tail call to an `in_s` callee, `needs_heap_join` rejects the join -> the enclosing
function evicts to the direct emitter, which handles `(inner (inner x))`
correctly.  (`needs_heap_join`'s existing `CT_TAILCALL` case caught only a KK_VAR
cps->cps call at the top level; a KK_RET cps->cps call is legitimate at a
function's top level but not inside a lifted join body -- exactly the gap.)
`continuation-substrate`, `contract-nested`, and the full suite are green.
