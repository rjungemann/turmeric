---
status: open (latent -- the exposing shape is a located hard error today, not a miscompile)
severity: low-medium (becomes a silent-wrong-answer miscompile the day the admission widens)
discovered: 2026-08-05
area: compiler (emit_cps_ir.c emit_reset; DK runtime chain layout)
---

# emit_reset still builds the pre-unification two-spine layout

## Summary

The spine unification
([cps-multishot-nontail-resume-inner-handle-drops-clause-rest](../archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md))
converted the **handle** continuation frame to a `DKK_RESUME_FRAME` whose
`next` is the real, borrowed enclosing chain. The **reset** continuation frame
(`emit_reset`, `src/compiler/emit_cps_ir.c`) was left on the old layout:

```c
DK *__pN = __dk_reap_keep(dk_prompt(1,
    dk_frame(<kname>, <env carrying __k>, dk_copy_enclosing_handlers(cur_k))));
```

-- a baked `__k` env pointer to the ORIGINAL enclosing chain, plus a
done-terminated marker copy as `next`. This is byte-for-byte the layout whose
copies escaped their delimiter in the handle case: any `dk_copy_range` copy of
this frame still jumps to the original chain, and the `next` spine dead-ends
at the markers.

## Why it is latent, not live (verified 2026-08-05)

The frame is only copied when a capture crosses the prompt+frame -- i.e. a
`perform` inside the `reset`'s delimited body whose handler encloses the
reset. Probing that shape (both single- and multi-shot):

```turmeric
(handle (+ 100 (reset (+ 1 (perform (Ask)))))
        (Ask [] ^multishot k) (+ (resume k 1) (resume k 10)))
```

evicts the enclosing function from the CPS subset, and the direct emitter
reports the located hard error "this effect operation has no lowering here"
-- for the single-shot variant too. So no wrong answer is reachable today; the
cost is an **expressiveness gap** (the interpreter runs the single-shot
variant fine, printing 108 for `(Ask [] k) (resume k 7)`; on the interpreter
side `reset` is a fiber boundary, so multi-shot across it is refused with the
"already been resumed" diagnostic -- direction 3 of
[turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md)).

Plain `shift` cannot expose the layout either: Turmeric shift is abortive
(`__dk_abort_body` -- the captured sub is never resumed), and dk_run_impl's
SHIFT delivery continues at `P->next` on the ORIGINAL chain, which is the real
frame, once.

## The trap

If the CPS admission for perform-inside-reset ever widens (Track A has been
widening exactly this kind of position), the handle miscompile reappears here
verbatim: the resumed copy jumps to the original chain via the baked env, the
enclosing continuation runs once per resume, and the marker `next` dead-ends
delivery. Nothing in the code marks this dependency today except this report.

## Fix directions

The same conversion the handle got, mechanically: lift the reset continuation
as `LH_RESUME_CONT` (threading its run-time `__kont` instead of a baked
`__cap->__k` / env-as-k), and install it as
`dk_prompt(1, dk_frame_resume_borrow(<kname>, <caps-only env>, cur_k))`. The
runtime side (`borrow_next`, `dk_frame_resume_borrow`, copy/free discipline)
already exists and is exercised by every handle. `emit_await`'s bounded path
(see [cps-await-cont-baked-env](cps-await-cont-baked-env.md)) is the same
family. Do the conversion BEFORE widening any perform-inside-reset admission;
ideally do it opportunistically the next time emit_reset is touched, since it
is small and the regression surface (reset/shift fixtures) is well covered by
the suite.
