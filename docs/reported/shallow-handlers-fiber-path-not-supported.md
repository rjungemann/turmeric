# `handle-shallow` is only lowered by the CPS/DK backend, not the fiber path

**Severity:** low (bounded expressiveness gap; rejected loudly, never
miscompiled). Tracks the graduation gate for the `cps-effects` experiment.

## Summary

F2 of the compiled-first-class-continuations follow-ups added shallow effect
handlers (`handle-shallow`, behind `--enable=cps-effects`). A shallow handler
lowers onto the DK substrate's no-reinstall path (`dk_handler_shallow`,
`src/runtime/cps_prompt.c`): `dk_perform` does **not** re-install the handler on
the captured sub-continuation, so a `resume` runs outside the handler -- the
handler-side twin of `shift0` vs `shift`.

The gap: only the CPS/DK backend (`src/compiler/emit_cps_ir.c`, `emit_handle`)
honors the shallow bit. That backend fires only for CPS-eligible handles -- in
practice a **single tail `perform`** in the handle body. Every handle shape that
would make deep-vs-shallow *observable* at the surface (two or more sequential
`perform`s, or a `perform` inside the resumed continuation) currently falls to
the older fiber dispatch path (`tur_effect_perform` / `emit_effects_handle`,
`src/compiler/emit_effects.c`), which is hardwired deep.

Consequence: for a single tail `perform`, deep and shallow are behaviourally
identical, so a surface program cannot yet demonstrate the difference. The
deep-vs-shallow reinstall distinction *is* pinned directly on the substrate in
`tests/cps_prompt_unit.c` (`dk-deep-handler-reinstall` /
`dk-shallow-handler-no-reinstall`).

## Current behaviour (not a miscompile)

A `handle-shallow` whose shape reaches the fiber emitter is **rejected** rather
than silently lowered as deep:

```
error: 'handle-shallow' is not supported for this handle shape yet: shallow
handlers are only lowered by the CPS/DK backend, which this handle does not reach
```

(See `emit_effects_handle`, the `h->shallow` guard in
`src/compiler/emit_effects.c`, and fixture
`tests/fixtures/errors/effect-shallow-fiber-shape/`.)

## Fix directions

Teach the fiber effect path about shallow handlers so an observable shallow
program compiles:

- The deep semantics in `tur_effect_perform` / the per-handle `__dispatch_<id>`
  loop come from the handler frame staying installed across `resume`
  (`src/compiler/emit_module.c`, ~`:8127` `tur_effect_perform`;
  `EffectHandlerFrame` chain). A shallow handler frame should be **popped from
  the chain before the resumed fiber continues**, so a re-`perform` of the same
  effect escapes to the enclosing handler (or is unhandled).
- Mirror the DK model: shallow = do not re-establish the handler for the
  resumed computation; deep = keep it. The bit is already threaded end to end
  (`HandleExpr.shallow` -> `CT_HANDLE.handle.shallow`); the fiber emitter just
  needs to consume it instead of rejecting.

This is the graduation gate for the `cps-effects` experiment
(`src/runtime/experiments.c`): graduate once the fiber path honors shallow and
the effect hot path measures neutral-or-better.

## Plan

`docs/upcoming/v1/compiled-first-class-continuations-followups-plan.md` (Phase F2).
