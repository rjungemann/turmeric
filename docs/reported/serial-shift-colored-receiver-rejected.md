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

## Correction to the fix direction, 2026-09-05

The root cause above is confirmed. Instrumenting the receiver-admission point on
the repro:

```
SSPROBE kf kind=EX_VAR name=__fn_1544 global=1 lifted=1 colored=1 tykind=TY_FN
```

-- the colored lambda is lifted to a global function VALUE, so
`marshal_named_receiver` rejects it on `is_global && callee_colored` and the U7
arm rejects it for not being an `EX_CLOSURE`, exactly as described.

**The fix direction, though, does not fix the soundness problem it names.** It
says to thread "the helper's own downstream chain (`subk`'s continuation)" as the
receiver's `DK *`. `subk` is the bodyfn's second parameter and is already in
hand, so the plumbing looks free -- but read what the driver builds
(`src/runtime/cps_prompt.c`, `dk_run_impl`, `DKK_SHIFT`):

```c
DK *sub = dk_copy_range(k->next, P);          /* frames shift -> prompt */
DK *tail = reinstall ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done())
                     : dk_done();
sub = dk_append(sub, tail);
intptr_t bodyval = k->body(k->body_env, sub); /* <- subk */
dk_free(sub);
if (to_root) return bodyval;
k = P->next;                                  /* <- the OUTER chain */
v = bodyval;
```

`subk`'s continuation is a **re-installed prompt followed by DONE**. Threading it
as the receiver's `__kont` would send a `perform` inside the receiver past that
prompt and into DONE -- escaping to root. That is the same class of failure as
the direct-entry wrapper's fresh DK root, which is the thing the restriction
exists to prevent. Different address, same escaped effect.

What a colored receiver actually needs is a `DK *` with two properties, and they
come from different places:

- **returning** must land the value as `bodyval` (the driver then delivers it to
  the prompt's outer continuation), and
- **performing** must reach the chain OUTSIDE the enclosing prompt -- `P->next`
  in the code above.

`P->next` is computed by the driver and **never handed to the body**: `DKBody` is
`intptr_t (*)(intptr_t env, DK *sub)`. So the change is not per-site plumbing in
`emit_cl_shift_bodyfn`; it is a widening of the `DKBody` contract (or some other
way for the body to reach the outward chain), which is why the restriction has
stood.

The precedent to model on is in the same switch, one case up:

```c
case DKK_RESUME_FRAME:
    /* A suspending continuation frame: hand it the run-time
     * downstream chain (k->next) and let it own delivery. */
    return k->rfn(k->env, v, k->next);
```

A frame that needs the downstream chain already gets it, through its own
callback type. A shift body that calls a colored receiver needs the same
treatment, and that -- not the emitter -- is where the work is.

Nothing else in the report changes: the workaround is unchanged, and the
restriction is still sound as it stands.
