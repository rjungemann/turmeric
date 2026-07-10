---
title: CPS backend -- effectful TY_FN callback params
status: scoping
description: The last major N6 fallback lever after the generic-monomorph work. A colored function that INVOKES an effectful fn-value parameter (a callback whose type carries an effect row, e.g. (fn [cstr] #{Write} nil)) cannot be CPS-emitted, because calling it must thread the DK continuation but a first-class fn value has no DK entry point. This doc scopes the mechanism, the core design problem (context-polymorphic DK/fiber calling convention for first-class closures), and why -- unlike G1-G3b -- there is no cheap path.
---

# CPS backend -- effectful TY_FN callback params

## The gap

An *effect-free* plain `TY_FN` param already CPS-emits (its call is a delegated
indirect call -- the callback runs on its own entry, no continuation). An
*effectful* callback -- a param whose fn type carries a non-empty effect row,
`(fn [cstr] #{Write} nil)` -- is deliberately rejected by `fn_sig_ok`
(`effect_row_is_empty` gate), so a function that takes and *invokes* one falls
back to fiber:

```turmeric
(defn call-writer [f : (fn [cstr] #fx{Write} nil)] #fx{Write} : nil
  (f "hi"))                              ; invoking f performs Write, escaping to the caller
```

## The core finding: fiber uses DYNAMIC dispatch, DK uses STATIC threading

On the **fiber** path an effectful callback is trivial: `f` is a bare `int64`
function pointer and the call is a plain C call `f("hi")` -- the `Write` it
performs propagates through the runtime's *global effect-handler chain*
(`tur_current_fiber->effect_handler_chain`) at run time. No continuation is
passed; effect dispatch is dynamic.

The **DK** machine threads continuations *statically*: `dk_perform(tag, arg, k)`
delivers to the handler reached through the explicit `k`. So for the DK machine
to call an effectful callback, that callback's function must accept the
continuation -- `f(args, k)` -- and `f` must be a pointer to a **DK-shaped**
function. A plain fn pointer (fiber shape, no `k`) cannot participate.

This is why effectful callbacks are categorically harder than the
generic-monomorph work (G1-G3b): those reused *named-function* machinery and DK
threading between named functions. Effectful callbacks need DK threading through
a *first-class value*.

## What a DK-callable closure needs

The lambda `(fn [s] (perform (Write s)))` is already lifted to a top-level
function (`__fn_1267(int64_t s)` in the emitted C), so it is classifiable. The
pieces:

1. **Emit a DK variant of the colored lifted closure**: `__fn_N__cps(args, k)`
   that performs `dk_perform(...)`. This reuses the `emit_cps_ir_try_fn`
   machinery *if* the lifted closure fn reaches it as a classifiable entry --
   which needs checking (lifted closures are synthesized during closure-lifting;
   whether they appear as program items `ensure_S` walks, or only in the emit
   stream, determines how they get classified).
2. **The indirect DK call**: in a colored (DK) function, invoking `f` emits
   `((R (*)(A..., DK *))f)(args, k)` -- threading the caller's continuation.
3. **fn_sig_ok**: admit an effectful `TY_FN` param.
4. **Effect taint**: the callback's effect must be clean (the callback, the
   higher-order fn, and the handler all DK) -- the same whole-program taint the
   G3b `mono_template` work established, extended to callback effects.

## The core design problem: context-polymorphic calling convention

Piece 2 hides the real difficulty. A first-class fn *value* is
**context-polymorphic in its calling convention**: the *same* closure, passed to
a DK higher-order function, must present its DK entry `__fn_N__cps` (takes `k`);
passed to fiber code, its fiber entry `__fn_N` (no `k`). The value's *type*
(`(fn [cstr] #{Write} nil)`) does not distinguish them.

Concretely, in

```turmeric
(defn run [] : int
  (handle (call-writer (fn [s] (perform (Write s))))   ; closure created here
    (Write [s] k) (do (println s) (resume k nil))))
```

if `run` and `call-writer` are DK, the closure passed to `call-writer` must be
the DK pointer; but the *same closure expression* in a fiber caller must be the
fiber pointer. Resolving this needs one of:

- **A fn-value ABI carrying both entries** -- e.g. `{ void *env; fiber_fn;
  dk_fn; }` -- so the callee picks the entry matching its machine. This grows the
  carrier (today a bare `int64` pointer for a plain `TY_FN`, or the 2-word
  `tur_poly_fn_t` for a poly carrier) and touches every closure-creation and
  indirect-call site in both emitters -- an ABI change to a pervasively-used
  representation.
- **Per-convention closure monomorphization** -- lift a colored closure twice
  (fiber `__fn_N`, DK `__fn_N__cps`) and pick the pointer at each creation site
  by the surrounding function's machine. Avoids growing the carrier but needs the
  closure-creation emit to be machine-aware and the taint to decide each closure's
  convention.

Either way it is a cross-cutting change to closure representation + creation +
indirect-call in both the direct and CPS emitters -- a new subsystem, materially
larger than G3b (which added a flag + routing to existing named-function paths).

## Also: some effectful-callback patterns are pre-existing DIRECT bugs

A callback that is both invoked and *handled within the same function* does not
even compile on the direct path today:

```turmeric
(defn run-with [f : (fn [] #fx{E} int)] : int
  (handle (f) (E [] k) (resume k 5)))        ; 'f' undeclared -- direct emitter error
```

So the DK work here also depends on the direct emitter's effectful-fn-param
handling being sound for the patterns targeted (the propagate-up pattern
`call-writer` does compile; the handle-in-same-fn pattern does not). Worth a
separate `docs/reported/` note if pursued.

## Recommendation

This is the largest remaining N6 lever and the only one requiring a new
subsystem (DK-callable first-class closures with a context-polymorphic calling
convention) rather than an extension of existing machinery. It carries real ABI
/ design risk to a pervasively-used representation, and the current behavior is
sound (fiber handles effectful callbacks correctly via dynamic dispatch). Unlike
G1-G3b it has no cheap path and no safe incremental slice -- the calling-
convention decision is global.

Recommendation: schedule it as a dedicated design-first effort (choose the
carrier-both-entries vs per-convention-monomorph approach, prototype the
closure-creation + indirect-call change behind the experimental flag, verify the
fn-value ABI change does not regress the suite), rather than fold it into an
incremental pass. The generic-monomorph lever (G1-G3b, landed) already moved the
dominant `sig-param TY_APP` surface; effectful callbacks are the next-largest but
heaviest remaining item.

## Depends on / reuses

- The G3b whole-program effect-taint over monomorphs (`mono_template`) -- the
  callback-effect taint would extend the same fixpoint.
- `emit_cps_ir_try_fn` + the DK emit machinery -- for the lifted-closure `__cps`
  body.
- Parent: [cps-backend-n6-fallback-removal-plan.md](cps-backend-n6-fallback-removal-plan.md).
