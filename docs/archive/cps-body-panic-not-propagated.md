---
title: A CPS-colored function kept running after its callee's caught panic
category: Archive
description: On the DK/CPS path every panic-signal check emitted only a comment ("ret ctype unknown; no propagation here"), so a function whose callee panicked under catch-unwind -- or which panicked itself -- ran the rest of its body AND its whole continuation before the catch saw the flag. Found 2026-09-11 while fixing defer-in-generic-hof-skipped-on-caught-panic; fixed the same day.
---

# A CPS-colored function kept running after its callee's caught panic

**RESOLVED 2026-09-11, the day it was found.** Filed straight into the archive
as the paper trail for the fix.

**Severity: high** -- a silent wrong answer, no diagnostic. Every side effect
after the panicking call ran (prints, writes, the continuation's whole rest of
the program up to the `catch-unwind`), and the interpreter did none of it.

## Repro

```turmeric
(defn inty [^fat body : (fn [] int)] : int
  (let [x (body)]
    (println "not reached")
    (+ x 1)))
(defn main [] : int
  (catch-unwind (fn [] : int (inty (fn [] (do (panic "v") 0)))))
  (println "done")
  0)
```

Compiled: `not reached` / `done`. Interpreted: `done`. Same for a `void`
function, for a chain of CPS calls (`outer` -> `inty`), for a direct
`(panic ...)` in the CPS body (which then delivered an UNINITIALISED temp to
its continuation), and for a handler case continuing after a `resume` whose
continuation had panicked.

## Root cause

`inty` takes a `^fat` thunk, so it is CPS-colored and emitted as
`inty__cps(body, DK *__kont)`. Its delegated direct calls go through
`emit_value`, whose panic-signal check keys on `ctx->current_fn_ret_ctype` --
which the CPS renderer never set, so `emit_panic_signal_return` took its
"D1a prototype limit" arm and emitted a comment. The CPS emitter's OWN call
sites (the cps->direct `CT_LETCALL` / `CT_TAILCALL` arms and `resume` via
`dk_invoke`) emitted no check at all. And even with a `return 0` in place, a
frame function returning into `dk_run_impl`'s loop would have had its value
handed to the NEXT frame.

The mono/generic "asymmetry" in the defer report was this bug: the mono twin
happened to be CPS-colored and fell through to the normal-exit defer fire.

## Fix

- `emit_cps_ir.c` pins `ctx->current_fn_ret_ctype = "int64_t"` for the whole
  CPS render (the `__cps` body and every helper it lifts return the int64 /
  intptr word), so the delegated check becomes `if (tur_panicking) return
  ((int64_t)0);`. A `return` from a `__cps` body without running `__kont` hands
  the C stack straight back to the direct-entry wrapper (cps->cps calls are C
  tail calls), whose direct caller carries its own per-call-site check; the
  abandoned continuation frames are reap-owned (`__dk_reap_node`), so nothing
  leaks.
- `cps_panic_check` (the same `emit_panic_signal_return`, now exported) after
  the cps->direct call in both arms and after a non-tail `resume`.
- `emit_dk_runtime.c`: `dk_run_impl` returns on `tur_panicking` after a
  `DKK_FRAME` fires or a shift body returns; `__dk_drive_bounded` /
  `__dk_drive_after` abandon this level's pending meta-stack deliveries
  (reaped / freed) and return so the wrapper's caller sees the flag.

Pinned by `tests/fixtures/defer-generic-hof-caught-panic` (which now takes the
CPS path for its mono twin and the direct path for the generic one, and the
nested-scope case) and exercised by `bt-scope-panic-undo`.

## Not fixed here

A CPS function reached through an effect `perform` whose handler case then
tail-resumes (`dk_tail_resume`, the E7 trampoline) is covered by the driver
checks above; the multi-shot `cloneable` / serial-context paths were not
separately probed.
