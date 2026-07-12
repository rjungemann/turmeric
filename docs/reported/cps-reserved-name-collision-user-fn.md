# A user function named `k` (a CPS-reserved identifier) miscompiles to a crash

**Severity: MEDIUM (segfault on valid code; niche -- requires naming a function
`k` / `t<N>` / `__*`).**

## Summary

A top-level function whose name collides with a CPS-synthesized identifier
(`k` the captured continuation, `t<N>` temporaries, `__*` internals) miscompiles
when it is referenced from a CPS-colored context: the emitted C name clashes with
the backend's synthesized local, so calls dispatch through the wrong value and
crash. Found while sweeping cloneable-shift receivers (D6a follow-up) -- a
receiver named `k` segfaults; renamed, it works.

## Minimal repro

```turmeric
(defn k [c : int] : int c)                       ;; named `k`
(defn make [] : int (cloneable-reset (+ 1 (cloneable-shift k 0))))
(defn main [] : int (println (tur_cloneable_cont_resume (make) 10)) 0)
```

- Expected: `11` (continuation `(+ 1 [])` resumed with 10).
- Actual: **Segmentation fault** at runtime. The generated C compiles cleanly;
  the crash is a semantic miscompile (the emitted function `k` is shadowed by the
  CPS backend's synthesized `k` continuation local at the call site).
- Renaming `k` -> any non-reserved name produces the correct `11`.

## Root cause (direction)

The CPS backend (`emit_cps_ir.c`) synthesizes locals named `k` (the continuation),
`t<N>` (temporaries), and `__*` (internals) without namespacing them away from
user identifiers. The N6 work added a *parameter*-name guard
(`param_name_clashes_cps`, see the cps-backend-indirect-call fatclosure notes)
that excludes a function whose **param** raw name collides -- but a **global
function name** (the cloneable/serial-shift receiver, or any callee) that collides
is not guarded, so it slips through and shadows.

## Fix directions

- Extend the `param_name_clashes_cps` candidacy guard to also reject (evict) a
  colored function that *references* a binding whose raw name is a CPS-reserved
  identifier, OR
- Namespace all CPS-synthesized locals behind a reserved prefix that the reader
  forbids in user identifiers (e.g. `__cps_k`, `__cps_t<N>`), so no collision is
  possible.

The eviction path is safe today (the function would fall to the direct emitter);
the namespacing fix is the more complete one.

## Notes

Pre-existing and independent of the emit_cps.c removal (D1-D6); surfaced by the
D6a sweep. Not a silent miscompile in the "wrong number" sense -- it crashes --
but a crash on valid code is still a defect.
