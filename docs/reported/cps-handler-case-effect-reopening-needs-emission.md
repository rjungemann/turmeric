# Effect re-opening (a handler case that performs an outer-handled effect) needs emission work

**Severity:** medium (blocks `effect-reopen` and the compound half of a couple other
BODY-STRUCT-OR-TAINT fixtures from the CPS/DK backend under `--enable=cps-tramp-resume`;
correctness is fine -- they run on the fiber). Not a shipping-backend regression.

**One-line:** `handle_case_ok` (src/compiler/emit_cps_ir.c ~1528) has no `CT_PERFORM` case,
so a handler CASE body that itself performs an effect handled by an ENCLOSING handler
("effect re-opening") evicts as BODY-STRUCT-OR-TAINT. Admitting it is NOT a gate change --
the emission does not yet support the interior perform (a bounded probe miscompiled).

## Minimal repro

```turmeric
(defeffect Log [msg :cstr] :nil)
(defeffect Write [msg :cstr] :nil)
(defn main [] : int
  (handle                                         ; outer: handles Write
    (handle                                       ; inner: handles Log
      (do (perform (Log "start")) 0)
      (Log [msg] k) (do (perform (Write msg)) (resume k 0)))   ; <-- Log case RE-OPENS Write
    (Write [msg] k) (do (println msg) (resume k 0)))
  0)
```

`main` evicts `[EVICT] BODY-STRUCT-OR-TAINT`. A nested handler WITHOUT re-opening (the Log
case does not perform Write) CPS-emits fine -- so the trigger is specifically the interior
`(perform (Write msg))` in the handler case body. Real fixture: `effect-reopen`
(`counted-sum` is fine in isolation; it evicts only because `main`'s re-opening taints the
shared effect).

## Why (probe result)

`handle_case_ok` admits APPCONT(KK_PROMPT)/LETVAL/LETPRIM(incl println)/LETCALL/RESUME/
LETRAW/CALLCC/IF and defaults to `false` -- there is no `CT_PERFORM` case, so a case body
performing an effect hits the default reject.

A BOUNDED probe (add a `CT_PERFORM` case to `handle_case_ok` that checks slot-ok args +
`collect_caps` + recurses `handle_case_ok` on the continuation) was tried. It ADMITTED the
repro but MISCOMPILED:

```
error: '__kont' undeclared (first use in this function)
```

So `emit_lifted -> emit_term`'s `CT_PERFORM` path, when the perform sits inside a LIFTED
handler-case frame, references a `__kont` continuation that is not in scope in that frame.
The lifted case frame does not thread the enclosing handler's DK continuation to the
interior perform. This is real EMISSION work, not just an admission-gate widening.

## Fix direction

The interior perform in a re-opening handler case must dispatch to the ENCLOSING handler's
DK prompt (the `cur_k` in effect at the dynamic point the case runs). The lifted case frame
(`emit_lifted`) needs to make the enclosing continuation available to `emit_term`'s
`CT_PERFORM` emission (the `__kont` the error names) -- e.g. thread the frame's runtime
`__kont` parameter into the perform lowering, the way a normal `f__cps(..., DK *__kont)`
body has it. Then mirror term_core_ok's `CT_PERFORM` guards (`perform_body_ok` /
`perform_cont_reset_ok`, slot-ok args, `collect_caps`) in `handle_case_ok`, but with the
continuation staying in the CASE context (it may resume the case's own `k`). Verify with
the repro (expect `start` printed once) + `effect-reopen` (`start`/`done`/`142`) + suite.
