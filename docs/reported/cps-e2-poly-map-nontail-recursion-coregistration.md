# E2/E1: effect-poly-map evicts on a mutual-recursion co-registration cycle (non-tail fn-value HOF + fn-value-arg recursion)

**Severity:** medium (a precise, pinned E2 boundary; correct on the fiber).
`effect-poly-map` stays SIG-TAINT under `--enable=cps-tramp-resume` -- the
effect-poly HOF `map-list` DK-lowers (`map_hylist__cps` is emitted) but its
fn-value param `f` never threads, so the callback `__fn_1283` and `main` ride the
fiber.

## The shape

```turmeric
(defn map-list [n : int f :(fn [int] #fx{e} int)] #fx{e} : int
  (if (> n 0)
    (+ (f n) (map-list (- n 1) f))       ; NON-TAIL (f n) AND non-tail recursion passing f
    0))
(defn main [] : int
  (handle (do (map-list 3 (fn [x] (do (perform (Log "tick")) x))) 0)
    (Log [msg] k) (do (println msg) (resume k nil))))
```

## Root cause (a 2-node mutual-recursion cycle the least-fixpoint registration can't seed)

In the emitted `map_hylist__cps`, both fn-value edges fall back to the fiber:
- `(f n)` emits `((int64_t (*)(int64_t))f)(n)` -- f's DIRECT entry (fresh root
  prompt), NOT a registry thread; so the callback's `Log` perform escapes.
- the recursion emits `map_hylist((n)-1, f)` -- the DIRECT map-list, not
  `map_hylist__cps(..., __kont)`.

`(f n)` threads only if `f` is a thread-param, i.e. `param_is_thread_safe(map-list,
f)` (emit_cps_ir.c) -- which requires EVERY fn-value flowing into `f` to be
`threadable_has`.  Those are:
1. `__fn_1283` (from `main`'s `(map-list 3 <lambda>)`), and
2. `f` itself (from the recursion `(map-list (- n 1) f)` -- a fn-value ARG at the
   SAME param position).

Edge 2 fails today: `param_is_thread_safe` requires the arg be a registered
lifted lambda (`a->kind == EX_VAR && threadable_has(a->as.var.binding)`); a bare
param `f` is neither.  And edge 1 is circular: `__fn_1283` is registered only if
`fn_value_threadable(__fn_1283)` counts its "pass to `map-list` at param f" use as
OK, which requires `f` to ALREADY be a thread-param.  So:

    f thread-safe  <=>  __fn_1283 threadable

Both are TRUE in the sound answer, but the least-fixpoint (single pass, start
empty) registers neither: round 1 sees f not-a-thread-param -> __fn_1283 not ok
-> unregistered -> f not thread-safe.  No seed breaks the cycle.

## Fix direction (greatest-fixpoint co-registration + self-param recursion arg)

1. **`param_is_thread_safe`**: treat a recursion self-arg -- the callee's OWN
   param `pi` passed back at position `pi` in a call to the same `fd` -- as
   trivially safe (it threads the same row-poly value, introduces no new
   fn-value).
2. **Registration as a GREATEST fixpoint**: start with ALL candidate lambdas +
   HOF params assumed threadable, then iteratively REMOVE any lambda with a
   non-thread use or any param with a definitely-unregistered arg, until stable.
   The `__fn_1283`/`f` pair (whose only risky interaction is with each other)
   then survives as co-threadable.  (Today's least-fixpoint in the emit_cps_ir.c
   registration loops is the wrong direction for a mutually-recursive HOF.)
3. Then `(f n)` threads via the registry (E2a nontail heap join) and the
   recursion emits `map_hylist__cps((n)-1, f, <join>)`.

Both flag-gated; flag-off byte-identical; FULL flag-on soundness sweep mandatory
(this is the E2 family where a blanket relaxation was reverted before).

## Context

The non-tail residual left after the E2 row-poly TAIL cluster
(docs/archive/cps-e2-rowpoly-fnvalue-threading-boundary.md) and the E2c
struct-field cluster (docs/archive/cps-e2c-struct-field-effectful-fnvalue-threading.md).
Related: `effect-ref` (owning `ref<T>` live across a perform -- needs
owning-capture + drop glue in the continuation frame;
`owning_dropped_before_control` at emit_cps_ir.c:1816 rejects it), `fh-discharge-row`
(a `with-handler` that discharges one effect and leaves a leftover -- `do-work`
is not colored so it fiber-emits and taints the leftover), `effect-nested`
(value-position nested handle -- see cps-toplevel-synthesized-main-bypasses-dk.md),
`effect-capture-k` (by-reference mutable capture).
