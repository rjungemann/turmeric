# CM3 transitive consumer-mono: a forwarding consumer's lens is not specialized

**RESOLVED.** Both fix-direction pieces landed. (1) `mono_specs.c`
`register_forwarding_walk` (run to a fixpoint in `mono_specs_resolve_program`
before the CM1 resolve) registers a spec for a forwarding lens param `(E, p)`
inheriting the callee consumer's functor/focus/whole, so the fixpoint resolves it
and the inner consumer inherits its set transitively. (2) `emit_expr.c`'s CM3
rewrite resolves a lens arg that IS the current clone's bound lens param to the
clone's concrete lens (the "singleton lens set"), so `(inner l ...)` inside a
forwarding clone rewrites to the inner consumer's matching clone; `emit_module.c`
emits a twin-less clone for a forwarding consumer (no `(l g s)` pin). Verified on
direct-forward, multi-level (fixpoint), and self-recursive (OQ #3 -- a clone calls
its OWN clone, emit terminates) shapes; fixture
`van-laarhoven-lens-wide-consumer-forward` (run + `expected.c`) pins the box-free
`tweak__lens_* -> set_px__lens_*` chain. CM4 is no longer blocked by this shape.

---

**Severity:** medium (a missed optimization today -- correct via Path A fallback;
becomes a **CM4 blocker**, since CM4 deletes the Path A wide branch this case
still relies on).

## One-line

A consumer that only *forwards* its wide lens param to another consumer (no
`(l g s)` pin of its own) never gets a mono spec, so CM1's fixpoint cannot
resolve the inner consumer's lens set through it and neither consumer is cloned
-- the call sites fall back to the Path A carrier.

## Minimal repro

`tests/fixtures/van-laarhoven-lens-wide-consumer-forward/input.tur`:

```turmeric
(defn set-px [l <forall-lens> b : int s : Point] : Point
  (run-id (l (fn [a : int] : (Identity int) (mk-id b)) s)))   ; has the (l g s) pin

(defn tweak  [l <forall-lens> b : int s : Point] : Point
  (set-px l b s))                                             ; forwards l -- NO pin

(defn main [] : int
  (let [p (make-struct Point :x 3 :y 4)]
    (tweak point-x 99 p) (tweak point-y 88 p) ...))
```

`emit-c` shows zero `__lens_` clones; `main` calls the carrier `tweak(...)`.
Output is correct (99/4/3/88) -- a safe fallback, not a miscompile.

## Root cause

- VBM1 registers an abstract mono spec only at an `(l g s)` van Laarhoven pin
  (`elab_call.c` -> `mono_spec_register`). `tweak`'s body `(set-px l b s)` has no
  such pin, so `tweak`'s lens param `l` has no spec.
- CM1's transitive fixpoint (`mono_specs.c` `resolve_walk`) adds a forwarded
  param's set to the callee's set *only when the forwarded param is itself a
  resolved spec* (`spec_for_binding(vb)`). With `tweak`'s `l` unregistered,
  `spec_for_binding` returns NULL, so `set-px`'s set stays empty and it is not
  cloned. `tweak`'s `l` is likewise never resolved, so `tweak` is not cloned.
- CM3's call rewrite (`emit_expr.c`, `case EX_CALL`) only fires when the site's
  lens arg is a *named concrete lens* in the callee's set; a forwarded `l`
  resolves to the param name, misses the set (`mono_spec_lens_clone_hash == 0`),
  and falls through to Path A.

## Fix direction

Two pieces, in order:

1. **Register a spec for a forwarding lens param.** At resolve time (or a VBM1
   extension), when a call `(C l ...)` passes enclosing-fn param `l` into a
   consumer `C`'s lens slot, register an abstract spec for `(E, l)` inheriting
   `C`'s functor/focus/whole and `lensparam_binding = l`. Then the fixpoint
   resolves `E`'s `l` from `E`'s own call sites and `C` inherits it transitively.
   (Additive + monotonic -- the fixpoint already tolerates growing `g_specs`;
   guard the registration so it cannot loop.)
2. **Per-clone forwarded-lens resolution (cheap).** In the CM3 rewrite, when the
   lens arg is an `EX_VAR` whose binding == the *current* clone's
   `consumer_lens_binding`, use that clone's `consumer_lens_name` instead of the
   var name -- so `(set-px l ...)` inside `tweak__lens_point_x` rewrites to
   `set_px__lens_<point-x>`. This is the "singleton lens set" the plan's CM3
   transitive bullet describes; it only bites once (1) makes `tweak` clonable.

## Why it blocks CM4

CM4 deletes the Path A box/unbox on the wide branch. A forwarding consumer that
still lands on Path A would lose its only lowering. Either land the transitive
rewrite before CM4, or have CM4's residual decision (R1/R2) explicitly cover the
"statically-forwarded but unspecialized" shape (distinct from the
runtime-selected-lens residual, which is genuinely non-static).
