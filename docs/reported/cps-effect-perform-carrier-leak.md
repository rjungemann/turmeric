# CPS effect lowering leaks per-perform carrier allocations (arg array + Tier-C value boxes)

**Severity:** low (bounded, per-perform/-resume-execution heap leak; not a
correctness bug). Keeps `requires.no-leak-check` markers on the affected
delimited-effect fixtures. Sibling of the just-fixed DK-node leak
(`docs/archive/cps-delimited-dk-node-leak.md`) -- same family: the CPS effect
lowering heap-allocates a carrier to cross the `intptr`-typed
`dk_perform`/`resume`/handler ABI and never frees it.

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

1. Free the multi-arg `__eargsN` array after `dk_perform` settles, the same way
   the perform continuation frame is now single-node freed
   (`docs/archive/cps-delimited-dk-node-leak.md`): capture the `dk_perform`
   result, `free(__eargsN)`, then return. The array is dead once the handler has
   read its slots (the handler runs synchronously inside `dk_perform`).
2. Tier-C boxes: free the box once its owner has consumed it -- the arg box after
   the handler unboxes it, the resume-value box after `resume` copies it out, the
   handler-return box after the caller reads it. Because a resume is multi-shot
   (the captured continuation may replay), a box that rides a *replayed* value
   must be reference-counted or copied per replay rather than freed on first use;
   scope the free to the single-shot boxes first and treat multi-shot boxes with
   the drop-glue machinery.
3. Interim: both are bounded per-execution leaks; the affected fixtures keep
   `requires.no-leak-check` until this is wired.

## Affected fixtures (retain `requires.no-leak-check`)

`cps-backend-multiarg-effect`, `cps-backend-multiarg-effect-float`,
`cps-backend-tierc-effect`, `cps-backend-tierc-effect-arg`.
