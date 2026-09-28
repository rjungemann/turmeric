# A serial-shift receiver that calls anything colored is rejected

> **RESOLVED 2026-09-28** -- for every receiver and leaf that no effect
> escapes, which is the shape the report was filed from (a guestbook receiver
> calling a template through a `(fn [cstr] cstr)` parameter), and by a smaller
> change than either fix direction below.
>
> The refusal was keyed on the COLORING bit. Coloring is conservative: a
> fn-value call colors a function and every caller, effect or no effect. And a
> colored function called through its direct-entry wrapper is not exotic --
> it is how every effect-free colored function is called already (a `helper`
> calling `apply1` through a fn value colors both, and the call stays direct).
> The fresh DK root the wrapper starts only matters to a perform that
> ESCAPES the callee, and whether one can is exactly what the declared and
> inferred effect rows say. So `marshal_named_receiver` (both families) and
> the three context-callee checks in `build_marshal_reset` now refuse a
> colored target only when `fn_effect_may_escape` (`src/passes/cps_ir.c`):
> a non-empty declared or inferred row, or no row at all. Colored-but-silent
> receivers and leaves are admitted and called through the direct entry --
> sound for the same reason as `apply1`.
>
> Two things fell out:
>
> - **A receiver typed `k : serial-cont`** lowers `k` to the int64 carrier,
>   but the shift body passed the DK chain as `void *` whatever the receiver
>   declared -- a `-Wint-conversion` (a hard error on GCC >= 14 and macOS
>   clang) for a closure receiver, and a call through a mismatched
>   function-pointer type for a named one. The admitted shapes made it
>   reachable from more places; `serial_recv_kty` (`emit_cps_ir.c`) now reads
>   the spelling off the receiver's declared parameter.
> - **The remaining refusal says why.** A receiver an effect escapes is still
>   `TUR-E0706`, but the message now names the receiver and the effect row
>   instead of blaming the context shape (`emit_effects_serial_shift`), and
>   `tur explain TUR-E0706` covers receivers.
>
> Pinned by `tests/fixtures/serial-shift-colored-receiver` (the repro; a named
> colored receiver; a capturing closure typed `serial-cont`; a receiver
> reaching a self-handled effect; a colored cloneable receiver; colored 1-arg,
> 2-arg and do-tail leaves -- each serial capture marshalled to bytes and back
> before it resumes) and `errors/serial-shift-receiver-effect-escapes`. The
> guestbook harness (`tests/run-guestbook.sh`) is unchanged, 10/0. The guides'
> "must be uncolored" rules now read "no effect may escape".
>
> **Not done, and filed separately:** a receiver or leaf whose effect DOES
> escape, to be handled by a handler around the reset. The correction below
> is right that it needs the `DKBody` contract widened (the body must be
> handed the chain outside the prompt), but it is not sufficient: the serial
> reset runs its chain standalone (`dk_run(chain, 0)` with the prompt's outer
> continuation `dk_done()`), so `P->next` would have to be linked to the
> enclosing function's `__kont` with the reset's rest lifted into a resume
> frame, as `emit_reset` does for `reset`; and a function taking a
> `serial-cont` parameter is not CPS-emittable at all today (`fn_sig_ok`
> refuses the `cont` param), so the receiver's perform has no lowering
> regardless. See
> [serial-receiver-effect-cannot-reach-enclosing-handler](../reported/serial-receiver-effect-cannot-reach-enclosing-handler.md).

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
