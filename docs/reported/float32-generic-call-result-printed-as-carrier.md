# A `float32` returned from a generic call is consumed as the raw int64 carrier

**Severity:** silent wrong answer. `7.1` prints as `1088631603`.
**Status:** OPEN.
**Found by:** probing the `float32` sibling of
[`method-result-float-spec-return-value-converts`](method-result-float-spec-return-value-converts.md),
which asked for exactly this check. Same disease, **opposite** asymmetry --
they are two different cells, not one.

## Repro -- no typeclass needed

```turmeric
(defn idf [A] [x : A] : A x)
(defn main [] : int
  (println (:: 7.1 float32))          ; 7.1         -- literal alone, correct
  (println (idf (:: 7.1 float32)))    ; 1088631603  -- WRONG
  0)
```

`1088631603` is `0x40E33333`, the IEEE-754 single-precision bit pattern of
`7.1`.

The `float64` twin is fine -- `(= (idf 7.1) 7.1)` is `true` -- so this is
specific to `float32`, and it needs only a generic identity function. That
makes it strictly smaller than the method-result cell it was found next to.

## Root cause: the producer bit-casts, the consumer never casts back

```c
static int64_t idf__spec__int64_t_float(float x) {
        return ((union { float s; int64_t d; }){.s = (x)}).d;   /* correct: bits */
}
...
int64_t __ps_159 = (idf__spec__int64_t_float(7.1));
printf("%lld\n", (long long)(__ps_159));                        /* prints the CARRIER */
```

The producing side is right: the spec clone bit-reinterprets the `float` into
the int64 carrier. The consuming side never reinterprets it back -- it binds
the carrier as `int64_t` and hands it straight to the integer `printf` arm.
The call's result type was resolved to the erased carrier rather than to
`float32`, so no carrier->concrete bridge was inserted at the consumer.

Contrast the `float64` path, which specializes to a **concrete** return
(`..._spec__double_double`) and so needs no unbox at all. The width is what
selects the different spec shape.

## Relationship to the sibling report

Both are producer/consumer disagreements about how a float crosses the int64
carrier, with the roles exactly swapped:

| | producer | consumer | result |
| --- | --- | --- | --- |
| [`method-result-float-...`](method-result-float-spec-return-value-converts.md) (float64) | converts by VALUE (`return self;`) | reinterprets BITS | `7.1` -> `3.45846e-323` |
| this report (float32) | reinterprets BITS (correct) | reads the carrier as an INT | `7.1` -> `1088631603` |

That symmetry is the useful part: a producer-side rule alone cannot fix
either, because in one the producer is already right. Both want the
convention **paired** at the point the entry point is selected -- increment
2's `bind`-cell resolution
(`docs/upcoming/repr-decision-function-plan.md`) is the template.

## Fix direction

Fix the **consumer**: a generic call whose spec returns the int64 carrier
while the call's resolved result type is `float32` needs the
carrier->concrete bridge at the call site.  `emit_carrier_bridge` already
emits it (`CK_CARRIER -> CK_CONCRETE` with `carrier_is_inline`, which covers
`TY_FLOAT32`), so this is a routing question -- which is why it is likely the
easier of the two cells.

Check `float32` **arguments** in the same sweep; only the result direction was
probed here.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- open cell: **`float32`
generic (carrier) call result -> concrete consumer**. The guide's existing
note that a float crossing the carrier "needs a bit reinterpret, not a
numeric conversion" is right and insufficient: it is also possible to
reinterpret correctly and then never unwrap, which is this bug.
