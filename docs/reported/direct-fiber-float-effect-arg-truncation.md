# Direct/fiber emitter: a float effect argument is truncated to int

**Severity:** medium (silent miscompile; wrong value, no crash). Mainline
direct/fiber effect codegen -- NOT CPS-backend-specific. Surfaced while adding
multi-arg effect support to the CPS backend (which handles floats correctly).

## Summary

When an effect declares a `:float` argument, `perform`ing it on the ordinary
direct/fiber effect path delivers a **truncated integer** to the handler case
instead of the float. The fractional part is lost. This affects both a
single-float-arg effect and a multi-float-arg effect. The CPS backend
(`--enable=cps-backend`) round-trips the same value correctly, which is how the
divergence was noticed.

## Minimal repro

```turmeric
(defeffect One [a :float] :float)
(defn f [] : float
  (handle
    (perform (One 7.1))
    (One [a] k) (resume k {a + 1.0})))
(defn main [] : int (println (f)) 0)
```

- `tur build repro.tur && ./repro`                     -> prints `8`   (WRONG: 7.1 truncated to 7, +1.0 = 8.0)
- `tur build --enable=cps-backend repro.tur && ./repro` -> prints `8.1` (correct)

A plain float print (`(println {3.5 + 7.1})`) is correct (`10.6`), so the bug is
specific to a float value **crossing an effect argument** on the fiber path, not
float printing or arithmetic.

## Root cause (direction)

The direct/fiber effect lowering stores the effect argument into its one-word
carrier slot without the `double`<->`int64` union bitcast that a float needs
(`slot_store`/`slot_load` in the CPS backend do this: store
`((union { double d; int64_t i; }){ .d = v }).i`, load the inverse). The fiber
path appears to cast the float straight to `int64_t` (a C float->int conversion,
which truncates) rather than reinterpreting the bits, so `7.1` becomes `7`.

Fix direction: at the fiber-path perform site, reinterpret a `:float` /
`:float64` arg's bits into the carrier slot (union bitcast, not a numeric cast),
and apply the inverse when the handler case binds the param -- mirroring
`slot_store`/`slot_load` in `src/compiler/emit_cps_ir.c`. `:float32` needs the
32-bit variant.

## Relationship to the CPS backend

Orthogonal. The CPS multi-arg effect slice
(`tests/fixtures/cps-backend-multiarg-effect-float`) asserts the CPS path's
correct `10.6`, precisely because the direct path is wrong here. When this
direct bug is fixed, the direct and CPS builds of that fixture will agree.
