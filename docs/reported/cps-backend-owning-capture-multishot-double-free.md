# CPS backend: owning (non-Copy) capture double-frees under a multi-shot continuation

**Severity:** medium (blocks the *non-`^borrow`* owning-capture sub-slice; no
miscompile today because the case falls back to the direct emitter).

**Partially resolved.** The safe subset -- a `^borrow` owning value -- now rides
the env by a shallow value copy with no retain/drop: the type checker guarantees
the function never consumes it, so the shared handle is never released here and
the refcount matches the direct path even under a multi-shot read (`cap_add` /
`fn_sig_ok` admit `is_borrow`; fixture `cps-backend-capture-borrow`). This report
stays open only for a *consumed* owning capture (the function drops/moves the
value), where the double-free below is real and still forces fallback.

## Summary

A continuation that captures an *owning* value (an `rc<T>` handle, or a
by-value aggregate with `needs_drop_glue`) cannot ride the N6.3 capture env by
the same value-copy path that scalars and owning-free by-value aggregates use.
The env is leaked and shared read-only across a multi-shot resume; that is sound
for Copy values but not for owning ones, because the continuation body *itself
contains the rc/drop* of the captured value (the drop-insertion pass sinks the
drop to the value's last use, which is inside the continuation). A single
clone-on-capture is then dropped once per resume -> refcount underflow /
double-free on the second shot.

## Minimal repro (conceptual -- currently falls back, so it is correct today)

```turmeric
(defeffect E [] :int)
(defn g [] : int (perform (E)))
(defn f [] : int
  (let [r (rc/of 5)]
    (let [v (handle (g) (E [] k) (resume k 100))]
      (let [c (rc/strong-count r)]
        (rc/drop r)          ; <-- drop sunk INTO the handle continuation
        (+ v c)))))
```

`--dump-cps` shows the handle continuation as:

```
let c  = rc/strong-count(r)   ; CT_LETRAW, captures r
let t1 = rc/drop(r)           ; CT_LETRAW, captures r  <-- the hazard
let t0 = (+ v c)
(k t0)
```

`collect_caps` correctly *bails* here (`cap_ty_ok` rejects `TY_RC` -- neither
`slot_ty` nor `slot_box_ty`), so `f` falls back to the direct emitter and is
correct. The report is about what it would take to *stop* falling back.

## Root cause

`src/compiler/emit_cps_ir.c`: the capture env is `malloc`'d, populated once at
`emit_cont_env` / `emit_perform` time, and never freed (leaked with the DK
nodes; multi-shot-safe *for Copy values only*, as the code comments state). An
owning capture would need either:

- **retain-on-copy + affine-continuation proof:** clone once into the env only
  when the continuation is provably invoked at most once (single-shot), so the
  in-continuation drop balances the clone; or
- **per-shot cloning:** clone the captured value at each continuation entry
  (not once at capture) so each resume gets its own reference to drop, with the
  env holding the "master" reference. Needs a Clone method / rc-increment for
  `rc<T>` and deep-clone glue for owning by-value products.

Both need machinery the backend does not have yet (a single-shot/affine
analysis, or a per-entry clone hook keyed off the captured type's Clone/drop
glue). This is the retain-on-copy + drop-glue work flagged as "remaining N6.3"
in `docs/upcoming/v1/cps-backend-n6-fallback-removal-plan.md`.

## Fix directions

Start with `rc<T>` handle captures (clone = `tur_rc_clone`, a refcount bump)
under a proven-single-shot continuation: emit the clone into the env, and let
the continuation's own sunk drop consume it. Detecting single-shot is the
gating analysis -- a handler case whose body `resume`s exactly once on the
static path is the common tractable shape.
