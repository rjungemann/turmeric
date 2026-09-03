# A `(fn [k] ...)` serial-shift receiver that captures nothing is rejected

**Severity: low** -- a surprising `TUR-E0706` with an easy workaround (name the
receiver). Found 2026-09-02 while rewriting the guestbook example.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn a [] : int
  (serial-reset (page "" (serial-shift (fn [k : serial-cont] : int (k 1)) 0))))
```

`tur check` -> `TUR-E0706: serial-shift context is not capturable`. The same
lambda capturing any enclosing local (`(fn [k] (do (println x) (k 1)))`)
elaborates to an `EX_CLOSURE` and is accepted; the same body as a top-level
`defn` is accepted as a named receiver.

## Root cause

`build_marshal_reset` (src/passes/cps_ir.c) admits a receiver that is either a
named uncolored top-level function (`marshal_named_receiver`) or an
`EX_CLOSURE` (U7 -- the emitter bakes the closure thunk into the per-site body
fn). A capture-free lambda elaborates to a plain `EX_FN`, which matches
neither arm and lands on the `else SK_REJECT()` (`TUR_TRACE_CORE=1` prints
the line).

## Fix direction

Treat an `EX_FN` receiver like an `EX_CLOSURE` with an empty environment on
the U7 path (the thunk exists; only the env pointer differs), or lift it to a
synthetic named receiver before the collector runs. Until then the guide says
"named function or capturing closure".
