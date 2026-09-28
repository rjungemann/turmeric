# A capturing closure passed to a dynamic call from a CPS-lowered function leaks its env

**RESOLVED 2026-09-28**, by two changes that landed the same day:

- **Statically** -- see [Resolution](#resolution-2026-09-28).  The env of a
  capturing lambda passed to a parameter the callee only invokes is freed,
  with or without a collector.  Pinned by
  `tests/fixtures/saffron-lambda-arg-env-freed` (leak-checked);
  `tests/fixtures/tailcall-dyn-leak` passes a capturing lambda again.
- **By the collector** a compiled single-unit `#lang saffron` program now
  allocates from (see
  [any-widen-stored-in-an-adt-field-has-no-owner](any-widen-stored-in-an-adt-field-has-no-owner.md)).
  Measured with the repro in a loop -- 600,000 `vec-fold`s, 3,000,000
  capturing closures through `apply-to` -- it runs in a 1.4 MB heap (175
  collections, 201 MB freed) and fits a 256 MiB address-space limit it dies
  under without the collector.  What the static drops still cannot own on
  plain malloc is
  [saffron-static-ownership-residue](../reported/saffron-static-ownership-residue.md).

**Severity: low** (a leak, bounded by the number of such calls; no wrong answer).
Found 2026-09-23 while landing proper-tail-calls T6; pre-existing on `main`
(reproduced with the T6 changes stashed).

## Repro

```turmeric
#lang saffron
(defn apply-to [g x] (g x))
(defn step [acc x] (apply-to (fn [y] (+ y x)) acc))
(defn main [] : int
  (println (vec-fold [1 2 3 4 5] 100 step))
  0)
```

Built under AddressSanitizer (`tests/run-leak-check.sh`'s flags), LeakSanitizer
reports `160 byte(s) leaked in 5 allocation(s)`, allocated in `step__cps`: one
32-byte closure env per `vec-fold` element.

## Where

`step` makes a dynamic call (through `apply-to`'s result being CPS-colored),
so it is CPS-lowered. The lambda captures `x`, so it is a heap fat closure
built inside `step__cps` and handed to `apply-to` as an argument. The direct
emitter frees a non-escaping closure env at its `let`'s scope end; the CPS
backend's `reap_env` path (`cps_closure_env_freeable`, `emit_cps_ir.c`) only
covers a leaf-admitted, provably non-escaping env, and a closure passed as a
call argument is neither, so nothing releases it.

## Fix directions

Register the env for the DK entry-boundary reap when the callee is known not
to retain it (the same "non-retaining sink" facts `fn-value-fat-normalization`
uses for stack boxes), or drop it after the consuming call the way the direct
emitter's argument-hoist drain does -- the CPS deferred-drop table already has
the shape (`cps_deferred_capture`).

`tests/fixtures/tailcall-dyn-leak` passes a top-level function instead of a
capturing lambda for exactly this reason; switch it back when this is fixed.

## Resolution (2026-09-28)

This took the report's first direction: register the env for the entry-boundary
reap when the callee is known not to keep it.  Three pieces, landed with
[dynamic-returned-closure-env-is-never-freed](dynamic-returned-closure-env-is-never-freed.md),
whose drop machinery they reuse.

1. **`any` parameters join `nonretain_param_mask`** (elab_fns.c,
   `elab_infer_nonretain_masks`).  The escape walk now reads a dynamic call of a
   parameter as an invocation, so `(defn apply-to [g x] (g x))` does not retain
   `g`.  The bit means the same for any payload: the body only invokes the
   value, reads its tag, or passes it to a slot that does the same.

2. **The lambda is hoisted.**  `arg_is_freeable_closure_source` (elab_call.c)
   admits a capturing lambda widened to `any`, or a call returning one
   (`expr_is_fresh_any_closure`), at a non-retaining slot.  The existing
   `hoist_borrowed_closure_args` then binds it to a `__borrowc_N` let, as it
   already did for a typed lambda at a `^borrow` slot.  `any_let_move_drop_to_use`
   leaves such a binding at scope exit: the at-use drop is `__tur_any_drop`,
   which passes a `"fn"` payload through untouched.

3. **The let is released.**  A direct caller drops it at scope exit.  In a
   CPS-lowered caller (`step__cps` in the repro) the `CT_LETRAW` binding it is
   marked `reap_any_env`, and `emit_letraw` registers the env with
   `__dk_reap_closure`, but only in the main body and only when
   `(void *)__kont != tur_tb_root`.  A cps->cps tail call hands the callee
   `__kont`, and the callee's own dynamic tail call of the lambda bounces it to
   the trampoline driver exactly when `__kont` is the root.  The driver then
   runs the lambda after this entry, and its reap, have returned.  When
   `__kont` is anything else, nothing reached from here can bounce out.  The C
   tail call is kept.

`saffron-lambda-arg-env-freed` covers the repro, a non-tail consumer, a direct
caller, and two bouncers entered from the driver.  Under
`tests/run-leak-check.sh` it runs clean.
