---
title: CPS backend -- effectful TY_FN callback params
status: landed
description: A colored function that INVOKES an effectful fn-value parameter (a callback whose type carries an effect row, e.g. (fn [cstr] #{Write} nil)) now CPS-emits. The initial scoping predicted a large DK-callable-closure subsystem; a spike disproved that -- the effect rides the fiber machine's dynamic dispatch transparently, so admitting the param is sound with no new mechanism.
---

# CPS backend -- effectful TY_FN callback params

## Outcome: a one-gate change, not a subsystem

The initial scoping of this doc predicted the largest, heaviest remaining N6
lever -- a new "DK-callable first-class closure" subsystem with a
context-polymorphic (DK-vs-fiber) calling convention. **A spike disproved that.**
The actual change is a single gate: `fn_sig_ok` (and `mono_sig_ok`) now admit an
effectful `TY_FN` param (`fn_param_ok = p->type.kind == TY_FN`, dropping the
`effect_row_is_empty` requirement). No CPS-IR change, no closure ABI change, no
routing.

## Why it is sound (the spike's finding)

The wrong assumption was that invoking an effectful callback must thread the DK
continuation. It does not. The **fiber effect machine dispatches DYNAMICALLY**
through a global handler chain (`tur_current_fiber->effect_handler_chain`), so an
effectful callback is a plain fiber call whose effect propagates with no
continuation passed. The CPS backend already **delegates** an indirect fn-param
call (a `CT_LETRAW` emitted as a plain call `f(args)`), so the effect rides the
fiber machine and never touches DK. That is sound because:

- **The whole-program taint keeps performer and handler co-fiber.** The effect is
  performed by the (fiber) callback body, which taints it, so any DK function
  handling that effect is evicted -- the handler stays fiber, reachable by the
  fiber chain. A DK function that would itself perform the effect *on DK* is
  likewise evicted (verified: a higher-order fn that both invokes an effectful
  callback and performs a second effect directly falls back to fiber, `apply-cb
  DK?=0`).
- **The DK higher-order function's continuation survives the fiber suspend.** Its
  frame lives on the fiber's C stack; a single-shot resume preserves it. Verified
  with real post-call work: `{(f) * 2} + base` = 27, apply-cb genuinely DK.
- **A fiber context is always present.** In a well-typed program an effectful
  callback is only reached within a handler for its effect, and that handler is
  fiber (by the taint), so `tur_current_fiber` is set when the callback performs.
- **Multi-shot is not a new hazard.** Resuming a continuation captured through a
  callback more than once is a hard error on BOTH paths (`TUR-E0201`), so it is
  out of scope here, not a regression.

## What landed

- `fn_sig_ok` / `mono_sig_ok`: admit an effectful `TY_FN` param.
- Fixtures: `cps-backend-effectful-callback` (higher-order `apply-cb` with real
  continuation work after the effectful call, `direct == cps == 27`);
  `cps-backend-fn-param-effectful` (repurposed from the old fallback guard into a
  positive test -- an effectful `#{Write}` callback invoked from a DK function
  that also handles a *different* effect on DK, `hi` / `5`).
- Verified across mixing DK+fiber effects, continuation-after-callback, named and
  nested callbacks. Full suite: 2040 passed, 0 failed.

## Bounds / related

- A callback that is invoked and *handled within the same function* does not
  compile on the direct path either (a pre-existing direct-emitter bug), tracked
  in
  [docs/reported/direct-effectful-fn-param-handled-in-same-fn.md](../../reported/direct-effectful-fn-param-handled-in-same-fn.md).
  The CPS path inherits that limit (it delegates to the direct emitter), so that
  shape is out of scope until the direct bug is fixed.
- True DK-native effectful callbacks (threading the DK continuation through a
  first-class value) remain unbuilt and unneeded: the fiber-dynamic-dispatch route
  is correct and simpler. The subsystem the initial scoping described is a
  non-goal.

## Depends on / reuses

- The whole-program effect-taint (`ensure_S`), which keeps the delegated effect
  co-fiber -- the load-bearing soundness mechanism.
- The existing `CT_LETRAW` indirect-call delegation.
- Parent: [cps-backend-n6-fallback-removal-plan.md](cps-backend-n6-fallback-removal-plan.md).
