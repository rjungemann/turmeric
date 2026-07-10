# Direct backend: capturing `escape`/`call-cc` in an effect handler case miscompiles

**Severity:** low (contrived nesting; a compile error, not a silent miscompile).

**Status:** partially resolved. The **shift-body** (and perform-continuation)
variant is fixed: `collect_free_vars` now descends into a `(call/cc f)`/`(escape
f)` receiver (`elab_core.c`, `EX_CALLCC` case -- both an `EX_CLOSURE` and a raw
`EX_FN` receiver), so the escape's enclosing captures are threaded into the
lifted helper env on the direct path, and the CT-IR backend admits these
positions (the `shift_body_ok` / `perform_body_ok` carve-out guards were
removed). The **effect handler case** variant below is still open.

## Remaining repro (handler case)

```turmeric
(defeffect E [] :int)
(defn g [] : int (perform (E)))
(defn f [n : int] : int
  (handle (g) (E [] k) (resume k (+ 1 (escape (fn [j] (j n)))))))
(defn main [] : int (println (f 5)) 0)
```

```
error: 'n_1271' undeclared (first use in this function)   // in __effect_handler_N
```

The escape receiver captures `n`, but the emitted handler function
`__effect_handler_N` never materializes `n` from its `__env`.

## Root cause

An effect handler case body is emitted as a fiber handler function whose
captures are collected by a **separate** walker, `collect_handle_captures`
(`src/compiler/emit_core.c:3168`) -- not the now-fixed `collect_free_vars`.
`collect_handle_captures` does not descend into an `EX_CALLCC` receiver, so the
escape's capture `n` is absent from the handler's capture set / `__env` and is
undeclared in the generated handler function. (Unlike `collect_free_vars`, this
walker also lacks nested-lambda param exclusion, so descending into the receiver
would need the same care -- fold an `EX_CLOSURE`'s captures, or recurse into a
raw `EX_FN` body excluding its params.)

## Interaction with the CPS backend unification (U2)

The CT-IR backend keeps a guard (`letraw_has_callcc`, used by `handle_case_ok`)
that evicts a callcc-bearing handler case to the direct emitter, so this shape
takes the direct path on both backends (`direct == cps`) rather than diverging.

## Fix directions

- Teach `collect_handle_captures` to descend into an `EX_CALLCC` receiver
  (folding an `EX_CLOSURE`'s captures, or recursing into a raw `EX_FN` body with
  its params excluded), mirroring the `collect_free_vars` fix; then drop the
  `handle_case_ok` guard so the shape lowers on DK directly.
- Or, longer term, retire the direct escape path (U7) and wire the escape
  captures into the CT-IR handler-case helper env.
