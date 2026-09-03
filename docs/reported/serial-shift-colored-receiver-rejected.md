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
restriction is about how the emitter calls it (`emit_cl_shift_bodyfn`,
src/compiler/emit_cps_ir.c): the shift-body helper calls the receiver as a
plain C function -- the named receiver through its fn pointer, a closure
through its thunk. A colored function's plain entry is its **direct-entry
wrapper**, which starts a fresh DK root; a `perform` inside it would then be
handled under that fresh root, not by the handler enclosing the reset --
the same escape the coloring pass guards against for address-taken
effectful functions (`g_addr_taken`). So the refusal is a soundness rule
today, not an oversight: admitting a colored receiver as-is would turn a
compile-time `TUR-E0706` into a run-time escaped effect.

## Fix direction

Call a colored receiver through its `__cps` entry from the shift-body
helper, threading the helper's own downstream chain (`subk`'s continuation)
as the receiver's `DK *` so a `perform` inside it reaches the enclosing
handler; the value it returns when it does not resume is the reset's result.
The shape already exists for colored *callees* of colored functions
(`CT_TAILCALL` with a `KK_VAR` continuation), so this is plumbing the
receiver call through the same path rather than the direct entry. Until then the guide says "the receiver, and
everything it calls, stays uncolored": in the guestbook that means the
receivers call the templates and the store directly rather than through a
`(fn [cstr] cstr)` parameter.
