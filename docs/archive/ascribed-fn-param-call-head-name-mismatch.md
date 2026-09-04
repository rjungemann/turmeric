# Calling an ascribed `:fn` param emits a call-head temp whose declaration and use disagree on its C name

**Severity: medium** -- a hard cc error (`'__call_head_N' undeclared`) on a
documented spelling, with no fixture covering it. Filed 2026-08-27, found
while building the regression fixture for
[fat-dispatch-wide-byvalue-aggregate-argument](fat-dispatch-wide-byvalue-aggregate-argument.md);
unrelated to aggregates -- a scalar reproduces it.

**Status: RESOLVED.** Fixed by the "sturdier" direction below, though not by
the mechanism the original write-up proposed -- see "Correction to the root
cause".

## Repro

```turmeric
(defn apply-un [f : fn v : int] : int
  ((:: f (fn [int] int)) v))

(defn mk [bump : int] : ptr<void>
  (let [lb bump]
    (:: (fn [x : int] (+ lb x)) :ptr<void>)))

(defn main [] : int
  (println (apply-un (mk 100) 6))
  0)
```

```
tur_poly_fn_t _un_uncall_unhead_un1344_1345 = f;   /* the DECLARATION */
... __call_head_1344.fn(__call_head_1344.env, ...) /* the USE */
error: '__call_head_1344' undeclared
```

## Root cause

`elab_call_head_expr` (`elab_call.c:1487`) hoists the callable head into a
synthetic binding named `__call_head_<N>`, and the two emission ends name it
through different rules:

- **Use** (`emit_call_name` -> `raw_name_for_binding`, `emit_core.c`): the
  raw path's "compiler-synthesized `__` names that are already pure C
  identifiers are emitted verbatim" rule keeps `__call_head_1344` as-is.
- **Declaration** (`name_for_binding`): lands on the id-suffixed mangling
  tail (`tur_mangle_append` + `_<id>`), which escapes every `_` as `_un` and
  appends the binding ordinal: `_un_uncall_unhead_un1344_1345`.

Same binding, two names. The `-Wimplicit` era would have linked anyway; today
it is a clean build break.

### Correction to the root cause

The original write-up said the use side reaches the verbatim rule because
"the binding's type is a non-boxed `TY_FN`, so `name_for_binding`'s
bare-function-reference branch returns the raw name". Measured, that is not
what happens, and the difference is what decides the fix.

The temp's type is **`TY_PTR_VOID` with `is_poly_fn` set** -- not `TY_FN` at
all (instrumented: `kind=6 is_poly_fn=1 is_fat=0 boxed=0`). So it misses
*every* raw-returning branch of `name_for_binding` -- parameter,
inline-C-named local, non-boxed `TY_FN` -- and falls through to the
id-suffixed mangler. The use side never consults `name_for_binding`: it goes
`emit_call_name` -> `raw_name_for_binding` directly.

The divergence is therefore not "one branch of one function picked wrong for
this type". It is that the two ends call **different functions**, and they
agree only when `name_for_binding` happens to delegate to
`raw_name_for_binding`. That coincidence holds for parameters, inline-C-named
locals, and non-boxed `TY_FN` locals -- and fails for everything else.

## Fix

`emit_call_name`'s fallback now routes a **local** callee through
`name_for_binding`, the same function that emitted its declaration; a global
keeps `raw_name_for_binding`, because a top-level defn's C name *is* the
function symbol.

```c
if (b && !b->is_global) {
    return name_for_binding(ctx, b);
}
return raw_name_for_binding(b);
```

This is the report's "sturdier" direction -- the two ends can no longer
diverge for any local binding, present or future -- and it changes no name
that already agreed: for every local shape where `name_for_binding` delegates
to `raw_name_for_binding`, it returns the identical string. Measured: **zero**
codegen-snapshot churn across 2741 fixtures.

### Why not the "cheapest" direction

The first suggestion -- make the DECLARATION apply the verbatim
short-circuit too, so both ends spell `__call_head_N` -- was implemented and
measured first. It is wrong, and instructively so.

The "`__`-prefixed pure C identifier" class is not only counter-bearing
compiler temps (`__call_head_%u`, `__fn_%u`, `__env_%u`). It also contains
**macro-template names with no gensym counter** -- `__v` and `__vw` from
`stdlib/vec.tur`'s `vec-of`:

```turmeric
`[__vw ~(first xs)]
  `[__v (vec-empty-like__ __vw)]
```

Their distinctness in emitted C comes from precisely the `_<id>` suffix that
the verbatim rule drops. Two nested `vec-of` expansions in one C scope would
collide. The change also churned **148 fixture snapshots** for a defect that
needed one class of binding touched.

The suite caught the churn but would not reliably have caught the collision:
the 148 failures were all `codegen mismatch`, none behavioral. The hazard was
latent, not demonstrated -- which is the argument for not shipping it.

## Coverage

`tests/fixtures/ascribed-fn-param-call-head` (new) exercises
`((:: f (fn [...] ...)) args)` on a bare `:fn` param, three ways:

- a **scalar** through the untyped `:fn` carrier -- the repro above;
- a **wide by-value aggregate** (`P`) through the same spelling -- the
  crossing `fat-dispatch-wide-byval-arg` had to avoid because of this bug;
- **two call-head temps in one function**, asserting their names stay
  distinct (the property the rejected fix would have put at risk).

It asserts values, not just that it builds: each closure must reach its
captured `lb`, so a head dispatching to the wrong thing prints a wrong number
rather than failing to compile.

`tests/fixtures/fat-dispatch-wide-byval-arg`'s header comment is updated -- it
documented this bug as the reason it stayed off that spelling, which is no
longer true.

## Verification

- `bash tests/run.sh` -- 2741 passed, 0 failed (2740 + the new fixture).
- `bash tests/run-turi.sh` -- 1879 passed, 0 failed.
- `TUR=./build-jit/tur bash tests/run-jit.sh` -- 2646 passed, 0 failed.
- `bash tests/run-leak-check.sh` -- 59 passed, 0 failed, 1 known-open
  (`weak-upgrade-after-drop`, `inline-c-option-carrier-box-leaks`,
  pre-existing).
- The repro prints `106`; declaration and use both spell
  `_un_uncall_unhead_un1370_1371`.
