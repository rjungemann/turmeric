# CPS lifted heap-join helper references the enclosing continuation `k` (contract-nested)

**Severity:** medium (build failure on a nested-contract program under the CPS
backend; the direct emitter compiles it).  Surfaced by the `cps-backend`
graduation.

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

## Status

Carried red.  One of three residual `cps-backend`-graduation failures that are
CPS lowering/emit bugs rather than emittable-subset ABI gaps (the eviction-gate
work closed the ABI-divergence class; see the direct-lowering-removal plan and
the graduation-readiness note).  The "reject in `needs_heap_join`" option would
fold this into the eviction path; the "carry `k` into the frame env" option
makes it native.
