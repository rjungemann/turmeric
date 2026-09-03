# A serial-shift receiver that calls anything colored is rejected

**Severity: low** -- a surprising `TUR-E0706` with an easy workaround (keep
the receiver's callees uncolored, or do the colored work elsewhere). Found
2026-09-02 while rewriting the guestbook example; first filed as "a
capture-free lambda receiver is rejected", which was the wrong diagnosis --
a capture-free lambda that calls only uncolored code is accepted.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn apply1 [^fat f : (fn [int] int) v : int] : int (f v))
;; helper is COLORED: it calls through a fn value.
(defn helper [k : serial-cont] : int (apply1 (fn [x : int] : int (k x)) 3))
(defn a [] : int
  (serial-reset (page "" (serial-shift (fn [k : serial-cont] : int (helper k)) 0))))
```

`tur check` -> `TUR-E0706: serial-shift context is not capturable`
(`TUR_TRACE_CORE=1` names the collector line: the receiver is neither a named
uncolored function nor an `EX_CLOSURE`). The same program with `helper`
uncolored (`(defn helper [k : serial-cont] : int (k 3))`) is accepted, and so
is `(serial-shift helper 0)` for that uncolored helper; naming the *colored*
helper is rejected too (`marshal_named_receiver` checks `callee_colored`).

## Root cause

`build_marshal_reset` (src/passes/cps_ir.c) admits a receiver that is a
named **uncolored** top-level function (`marshal_named_receiver`) or an
`EX_CLOSURE` (U7 -- the emitter bakes the closure thunk into the per-site
body fn). Coloring propagates backward through the call graph, so a lambda
that calls a colored function is itself colored, is lifted as a colored
function value rather than a plain closure, and matches neither arm.

The receiver runs exactly once, at capture time, and is never marshalled, so
nothing about the *continuation* requires it to be uncolored. The
restriction is about how the emitter calls it: as a direct C function, with
no continuation to thread into a `__cps` entry.

## Fix direction

Call a colored receiver through its `__cps` entry with a fresh continuation
at the reset site (the value it returns is the reset's result when it does
not resume), or lift the colored receiver's body into a delegated
(`CT_LETRAW`-style) region. Until then the guide says "the receiver, and
everything it calls, stays uncolored": in the guestbook that means the
receivers call the templates and the store directly rather than through a
`(fn [cstr] cstr)` parameter.
