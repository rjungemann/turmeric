# Bare `^fat` sink + poly box: slot-0 int64 shim vs `double` invoke cast -- round-trips only by xmm0 luck

> **Status:** Fixed (2026-06-04) -- fix direction 1, see Resolution below.

**One-line summary:** When a typeclass-method closure (`tur_poly_fn_t`) is
boxed via `EX_POLY_TO_FAT` and handed to a **bare** `^fat` sink (one with
no `:(fn [...] :T)` annotation, whose `:float` result is recovered by the
bare-fat result-type-inference retype pass), slot 0 of the box keeps the
int64 `__tur_poly_to_fat1` shim while the sink invokes it through a
`double (*)(void *, double)` cast. The two ABIs disagree; the value
round-trips only because `xmm0` is incidentally preserved across the
integer-ABI thunk hops.

**Severity:** Latent silent miscompile (register-class mismatch). Currently
masked -- a `:float` argument survives purely because no intervening thunk
clobbers `xmm0` -- which is exactly the "works by luck because the register
classes happen to match" defect class. No crash; will produce a wrong
number the moment the thunk chain touches `xmm0` (a different ABI, an
optimizer that reuses the register, or a non-float second argument).

## Context

Found while auditing the remaining type-passing gaps after
[poly-to-fat-typed-shim-plan.md](../upcoming/poly-to-fat-typed-shim-plan.md)
and its follow-up fix
([poly-wrapper-forces-int64-args-non-int-fat-sink.md](poly-wrapper-forces-int64-args-non-int-fat-sink.md)).
Those made the **annotated** `^fat f :(fn [:float] #{} :float)` sink correct
by threading the sink's declared fn signature onto the `EX_POLY_TO_FAT`
node (`poly_to_fat_.sink_fn_type`) so slot 0 gets the typed
`__tur_poly_to_fat1_double_double` shim. A **bare** `^fat` sink has no such
declared fn signature at the box site, so the threading falls back to NULL
and the int64 shim is kept -- but the sink still ends up invoking through a
typed `double` cast (re-stamped later by the bare-fat retype pass).

## Minimal repro

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")

;; BARE ^fat sink (no :(fn ...) annotation); :float result is inferred.
(defn call-bare [^fat g x : float] : float (g x))

(defstruct BoxW [A] (raw :int))

(definstance Functor [BoxW]
  (fmap [container fn] (call-bare fn 3.5)))

(defn main [] : int
  (let [k 0.0
        b (:: (make-struct BoxW (schema/int)) (BoxW int))]
    (println (.fmap b (fn [x : float] : float (+ (* x 2.0) k)))))  ; 3.5*2 = 7
  0)
```

- **Observed:** prints `7` -- but *by luck* (see analysis); slot 0 is the
  int64 shim, the invoke casts to `double`.
- **Expected (robustly):** prints `7` because slot 0 carries the typed
  `__tur_poly_to_fat1_double_double` shim matching the `double` invoke cast.

Replacing `call-bare` with the annotated form
`(defn call-ff [^fat f :(fn [:float] #{} :float) x : float] : float (f x))`
selects the typed shim and is robust -- that is the passing fixture
`tests/fixtures/poly-to-fat-float-roundtrip/`.

## Codegen evidence

The poly box stores the **int64** shim at slot 0:

```c
__t24[0] = (int64_t)(intptr_t)__tur_poly_to_fat1;   /* not __tur_poly_to_fat1_double_double */
```

while the bare sink invokes slot 0 through a **double** thunk cast:

```c
static double call_bare(void * g, double x) {
    return (*( tur_thunk_double_double_t *)(g))(g, x);   /* double (*)(void *, double) */
}
```

`tur_thunk_double_double_t` is `double (*)(void *, double)`; slot 0 holds
`__tur_poly_to_fat1`, which is `int64_t (*)(void *, int64_t)`. The types do
not match.

## Why it round-trips anyway (the luck)

`call_bare` is called with `g` in `rdi` and `3.5` in `xmm0` (double ABI).
Control reaches `__tur_poly_to_fat1(void *__e, int64_t a0)`, which reads
`__e` from `rdi` (correct) and `a0` from `rsi` (garbage -- the float was in
`xmm0`, never in `rsi`). It forwards `a0` to the real closure thunk, again
through the int64 carrier cast, so the real thunk's integer arg slot is
also garbage. But the real closure thunk is genuinely
`double (*)(void *, double)` and reads its argument from **`xmm0`** -- which
still holds `3.5`, because nothing in the two integer-ABI hops touched the
SSE register file. So the correct value arrives at the closure by accident.

The instant any thunk in the chain performs floating-point work (or the
platform/optimizer reuses `xmm0`, or a second non-float argument shifts the
register assignment) the masked mismatch becomes a visible wrong number.

## Root cause

`EX_POLY_TO_FAT`'s slot-0 selection
(`src/compiler/emit_expr.c`, the `poly_shim`/`sink_fn_type` block) depends
on `poly_to_fat_.sink_fn_type` being a concrete `TY_FN`, which is threaded
at the box site in `src/compiler/elab_call.c` from
`fn_type.as.fn.arg_full_types[fn_arg_idx_fat]`. For a **bare** `^fat`
parameter that slot is not a `TY_FN` (the param has no declared fn
signature), so `sink_fn_type` is NULL and the int64 `__tur_poly_to_fat1`
shim is kept. The bare-fat result-type-inference retype pass
(see [bare-fat-result-type-inference-plan / bare-fat-result miscompile
notes]) later re-stamps the **invocation** to the typed `double` cast, but
it does not revisit the already-lowered poly box to upgrade slot 0. The box
site and the invoke site therefore disagree on the slot-0 ABI.

This is a phase-ordering gap, not a logic error in either pass in
isolation: the typed-shim selection happens at lowering, before the
bare-fat result type is known.

## Proposed fix directions

1. **Thread the bare sink's inferred result/arg type to the box site.**
   When the bare-fat retype pass infers a non-int64 result (or argument)
   for a `^fat` parameter, propagate that inferred signature to any
   `EX_POLY_TO_FAT` (and `EX_FN_TO_FAT`) node feeding that parameter so its
   slot-0 shim is recomputed via `ensure_typed_poly_to_fat`
   (`ensure_typed_fatshim`). This makes the box and the invoke agree.

2. **Re-stamp the poly box at retype time.** If the retype pass already
   rewrites the call site, have it also rewrite the matching
   `poly_to_fat_.sink_fn_type` (or directly the emitted slot-0 selection)
   so the typed shim is chosen. Equivalent to #1, located in the retype
   pass rather than at lowering.

3. **Conservative defuse:** when a poly box would be slot-0-int64 but is
   consumed by a sink whose invoke is later retyped to a non-int64
   typed-thunk cast, emit a hard `TUR-E` ("a typeclass-method closure with
   a non-int64 signature must reach an annotated `^fat` sink; add a
   `:(fn [...] :T)` annotation"). Turns the luck-masked mismatch into a
   clear compile error and points at the working annotated form.

Directions 1/2 are the real fixes (make the bare form behave like the
annotated form); 3 is the cheap immediate defuse.

## How to validate a fix

- The repro selects `__tur_poly_to_fat1_double_double` at slot 0 (grep the
  emitted C), not `__tur_poly_to_fat1`, and prints `7`.
- A stress variant whose thunk chain performs intervening float work still
  prints the correct value (proves it no longer depends on xmm0 survival).
- `tests/fixtures/poly-to-fat-float-roundtrip/` (annotated) and
  `tests/fixtures/poly-to-fat-float-named-fn/` stay green.
- `bash tests/run.sh` clean; int64/pointer poly boxes churn-free.

## Resolution (fix direction 1)

Implemented by threading the bare sink's inferred fn signature to the box
site, exactly as the annotated form already does.

1. **Synthesize the bare-`^fat` param's fn signature** (`src/compiler/elab_fns.c`).
   When the bare-fat result-type-inference pass retypes a `^fat` param's
   tail call to a non-int register class, recover that invoke's argument
   kinds (`bare_fat_tail_call_arg_kinds`, mirroring the tail walk) and record
   `(fn [args] : R)` on the function type's `arg_full_types[param_idx]`. Done
   in both the `elab_defn` and `elab_fn` (lambda) paths. The existing box
   site (`elab_call.c`, `poly_to_fat_.sink_fn_type`) then reads that `TY_FN`
   and selects `ensure_typed_poly_to_fat` -> the typed
   `__tur_poly_to_fat1_double_double` slot-0 shim.

2. **Keep the `^fat` param's *emitted* C type as the carrier**
   (`src/compiler/emit_fns.c`, `src/compiler/emit_module.c`). `arg_full_types`
   is overloaded: it is also the source for a parameter's own emitted C type.
   A `^fat` param is a fat-closure *carrier* handle (`void *` / `int64_t`),
   never a by-value fn, so the four param-type emit sites (definition, CPS
   definition, forward decl, CPS forward decl) now ignore `arg_full_types`
   for an `is_fat` param and emit the carrier from `param_types` -- otherwise
   the synthesized `(fn ...)` would leak in as the param's type
   (`type_c_name(fn)` lowers to the *result* type, e.g. `double g`). The
   annotated form was already immune (it hits the `param_types[i].kind ==
   TY_FN -> int64_t` branch earlier); this makes the bare form behave the
   same.

Result: for `call-bare [^fat g x : float] : float (g x)` the box now stores
`__tur_poly_to_fat1_double_double` at slot 0 and the sink stays
`call_bare(void *g, double x)` -- ABI-consistent by construction, no longer
dependent on xmm0 survival.

**Regression coverage:** `tests/fixtures/poly-to-fat-bare-fat-sink/`
(capturing float closure through a typeclass method into a bare `^fat`
sink; prints `7`, slot 0 = `__tur_poly_to_fat1_double_double`). The existing
`tests/fixtures/bare-fat-float-result/` (bare `^fat` float sink without a
poly box) stays green and churn-free. `bash tests/run.sh`: 1352 passed,
0 failed.
