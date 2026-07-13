# CPS effect lowering leaks per-perform carrier allocations (arg array + Tier-C value boxes)

**Severity:** low (bounded, per-perform/-resume-execution heap leak; not a
correctness bug). Sibling of the fixed DK-node leak
(`docs/archive/cps-delimited-dk-node-leak.md`) -- same family: the effect
lowering heap-allocates a carrier to cross the `intptr`-typed
`dk_perform`/`resume`/handler ABI and never frees it.

> **PARTIALLY RESOLVED (2026-07-12).** The **DK-path** carriers are fixed: the
> multi-arg `__eargs` array and any boxed (Tier-C) argument riding `dk_perform`
> are now registered in the per-run reap list (`__dk_reap_ptr`) and freed at the
> outermost entry boundary, exactly like the DK env structs. A new
> `slot_store_reap` helper (`emit_cps_ir.c`) wraps only the boxes read with
> `slot_load(consume=false)` (shared read-only across a multi-shot resume, so
> never consume-freed at a load site -- reaping them cannot double-free). This
> clears `cps-backend-multiarg-effect`, `-multiarg-effect-float`, and
> `-tierc-effect-arg` (markers dropped).
>
> **Still open:** the **fiber-path** Tier-C boxes (`cps-backend-tierc-effect`).
> That fixture's effect lowers to the ucontext **fiber** runtime
> (`tur_effect_cont_resume`, `emit_effects.c`), whose `eff_box_expr` boxes the
> resume value / handler result and whose `eff_slot_load` never frees them, so
> they leak. Reaping them with `__dk_reap_ptr` is **not** safe in general: the
> DK reap prelude is emitted only when the program uses DK/CPS delimited control
> (`emit_module.c:6966`), so a **pure-fiber** effect program would reference an
> undefined `__dk_reap_ptr`. The fiber runtime needs its own box reclamation
> (free on fiber/handler teardown, honoring multi-shot resume) -- a separate
> change in `emit_effects.c` / the fiber runtime. `cps-backend-tierc-effect`
> keeps `requires.no-leak-check` until then.

## Summary

When an effect crosses the perform/resume boundary, the CPS effect lowering
mallocs a carrier and hands it through the int64 ABI, then never reclaims it:

1. **Multi-arg argument array.** An effect with >1 argument marshals its args
   into a heap `int64_t[]` (`__eargsN`) passed as the single `dk_perform` arg.
   The array is never freed. One leak per perform of a multi-arg effect.

2. **Tier-C value boxes.** A non-scalar effect argument / resume value / handler
   result (a struct or heap-boxed ADT) is boxed with `malloc(sizeof(T))` to ride
   the `intptr` slot (`__bx` / `__eb`). The box is never freed. One leak per
   boxed value crossing the boundary (arg box, resume-value box, handler-return
   box).

## Minimal repros

Multi-arg array (`cps-backend-multiarg-effect`, `-float`):

```turmeric
(defeffect Sum3 [a :int b :int c :int] :int)
(defn add3 [base : int] : int
  (handle
    {(perform (Sum3 1 2 3)) + base}
    (Sum3 [a b c] k) (resume k {{{a * 100} + {b * 10}} + c})))
(defn main [] : int (println (add3 1000)) 0)
```

LSan: `24 byte(s) leaked` from `add3__cps` (the `malloc(3 * sizeof(int64_t))`).

Tier-C value box (`cps-backend-tierc-effect`, `-arg`):

```turmeric
(defstruct Pr [first : int second : int])
(defeffect Get [] :Pr)
(defn use-get [] : Pr (perform (Get)))
(defn run [] : Pr
  (handle (use-get) (Get [] k) (resume k (make-struct Pr :first 32 :second 10))))
(defn main [] : int
  (let [p (run)] (println (+ (.first p) (.second p)))) 0)
```

LSan: two `16 byte(s)` leaks from `__effect_handler_*` (the resume-value box and
the handler-return box); the `-arg` variant leaks the perform-arg box.

## Root cause

- Multi-arg array: `src/compiler/emit_cps_ir.c:3428` in `emit_perform` --
  `int64_t *__eargsN = malloc(n * sizeof(int64_t))`; the array is passed to
  `dk_perform` and never freed. (The 1-arg path passes a scalar, no allocation,
  so it does not leak.)
- Tier-C boxes: `slot_store` boxes non-scalar values at
  `src/compiler/emit_cps_ir.c:204` (`__bx`), and `eff_box_expr` at
  `src/compiler/emit_effects.c:36` (`__eb`), for the perform arg, the resume
  value, and the handler result. None are freed after the value is consumed.

The handler case reads the arg array / unboxes the value once (see the
`__eargs`/`__eb` readback in the lifted handler at `emit_cps_ir.c:2650`), after
which the carrier is dead -- but no free is emitted.

## Fix directions

1. **DONE (DK path).** The `__eargsN` array and any boxed `dk_perform` argument
   are registered with `__dk_reap_ptr` at construction and freed at the outermost
   entry boundary (the per-run reap list from
   `docs/archive/cps-delimited-dk-node-leak.md`). Boundary reaping is safe for a
   multi-shot resume: each perform execution allocates a fresh carrier, registers
   it once, and every replay reads it read-only before the top-level `dk_run`
   settles. Only boxes read with `slot_load(consume=false)` are wrapped
   (`slot_store_reap`), so the entry-return box that `slot_load(consume=true)`
   frees is never double-freed.
2. **OPEN (fiber path).** The `emit_effects.c` boxes (`eff_box_expr`, resume
   value / handler result) leak the same way but cannot reuse `__dk_reap_ptr`:
   the reap prelude is gated on DK/CPS delimited-control usage
   (`emit_module.c:6966`), so a pure-fiber effect program would not have it. Give
   the fiber runtime its own box reclamation -- free the box on fiber/handler
   teardown, honoring multi-shot resume (reference-count or copy-per-replay a box
   that rides a replayed value rather than freeing it on first read).

## Affected fixtures

- Resolved (marker dropped): `cps-backend-multiarg-effect`,
  `cps-backend-multiarg-effect-float`, `cps-backend-tierc-effect-arg`.
- Open (retains `requires.no-leak-check`): `cps-backend-tierc-effect` (fiber
  path).
