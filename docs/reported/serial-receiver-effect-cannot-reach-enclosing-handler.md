# A serial-shift receiver's effect cannot reach a handler around the reset

**Severity: low.** A compile-time refusal (`TUR-E0706`, naming the receiver
and the effect), not a wrong answer; `tur --interpret` runs the same program.
The residue of
[serial-shift-colored-receiver-rejected](../archive/serial-shift-colored-receiver-rejected.md),
which admitted every colored receiver and leaf that no effect escapes.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defeffect Ask [] :int)
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn asks [] : int (+ (perform (Ask)) 1))
(defn recv-e [k : serial-cont] : int (k (asks)))       ; Ask escapes recv-e
(defn run [] : int
  (serial-reset (page "" (serial-shift recv-e 0))))
(defn main [] : int
  (println (handle (run) (Ask [] r) (resume r 41)))    ; interpreter: 43
  0)
```

Compiled: `TUR-E0706: serial-shift receiver 'recv-e' performs an effect that
escapes it ({Ask})` (plus a "this effect operation has no lowering here" on the
`perform`, for the second reason below). Pinned as
`tests/fixtures/errors/serial-shift-receiver-effect-escapes`. The same holds for
a context callee (leaf) an effect escapes, with the generic E0706 message.

## Why -- three independent gaps, all needed

1. **The body cannot reach the outward chain.** The shift body calls the
   receiver; for its perform to find a handler it needs a `DK *` whose tail is
   the chain OUTSIDE the prompt (`P->next` in `dk_run_impl`'s `DKK_SHIFT`
   case), which the driver computes and never hands over: `DKBody` is
   `(intptr_t env, DK *sub)`. `DKK_RESUME_FRAME` is the precedent -- a node
   kind whose callback is handed `k->next` and owns delivery. The archived
   report's 2026-09-05 correction has the detail.
2. **The serial reset runs its chain standalone.** `emit_cloneable`'s serial
   arm builds `dk_shift(... frames ... dk_prompt(1, dk_done()))` and runs it
   with `x = dk_run(chain, 0)` in the middle of the enclosing function, so
   `P->next` is `dk_done()` even when the enclosing function is colored and has
   a `__kont`. It would need `emit_reset`'s shape: the reset's rest lifted
   into a resume frame chained to `cur_k`, the chain run in tail position --
   and the enclosing function would need the receiver's effects in its row so
   classification threads it rather than evicting it.
3. **A `serial-cont` parameter is not CPS-emittable.** `fn_sig_ok`
   (`src/compiler/emit_cps_ir.c`) refuses the `cont` param, so an effectful
   receiver is SIG-REJECT and its perform has no lowering at all -- before the
   reset is even considered.

## Workaround

Handle the effect inside the receiver (or in a function it calls), or
perform it outside the `serial-reset`. A receiver that only calls through
function values, or handles its own effects, is accepted.
