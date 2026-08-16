# A float32-ascribed literal in comparison position emits as the double literal

**Severity:** silent wrong answer, engine-divergent -- compiled says `false`
where turi says `true`.
**Status: FIXED 2026-08-16, same day as filed.**  A float LITERAL ascribed
to a float kind is now retyped in place in the ascribe elaborator
(`elab_types.c`) -- `(:: 7.1 float32)` becomes exactly what `7.1f32` already
was, so emit renders it through `atom_float32` as a single-precision
constant and no mixed-width promotion occurs.  Scope is deliberately the
literal only: a non-literal float64 expression ascribed to float32 keeps the
erased-ascription behavior, because narrowing a runtime value is a
representation decision this fix does not take.  Pinned in
`tests/fixtures/float32-generic-call-result` (equality at 7.1 through both
the ascribed and suffixed spellings); suite 2600/0, compiled and turi agree,
fuzzer seeds 9401/9402 clean.

Originally filed as OPEN, pre-existing (reproduces on the compiler before the
2026-08-16 float32 generic-result fix; monomorphic repro, no generics
involved).
**Found by:** pinning the fixture for
[`float32-generic-call-result-printed-as-carrier`](../archive/float32-generic-call-result-printed-as-carrier.md)
-- its equality row went `false` on a shape the fix demonstrably handles, and
minimizing subtracted the fix from the repro entirely.

## Minimal repro -- no generics, no typeclass

```turmeric
(defn mono [x : float32] : float32 x)
(defn main [] : int
  (println (= (mono (:: 7.1 float32)) (:: 7.1 float32)))  ; compiled: false, turi: true
  0)
```

## Root cause

The emitted comparison is

```c
float __ps = (mono(7.1));
puts(((__ps) == (7.1)) ? "true" : "false");
```

Both operands were declared `float32` at the Turmeric level, but the RHS
ascribed literal `(:: 7.1 float32)` is emitted as the bare **double** literal
`7.1`.  C's usual arithmetic conversions then promote the `float` LHS to
`double`: `(double)7.1f` is `7.099999904632568...`, which is not `7.1`, so
the comparison is false for any value not exactly representable in single
precision.  (`7.5` compares true; that is the representability line, not a
fix.)

The same emission presumably feeds arithmetic, not just `=` -- a
`(+ (:: 0.1 float32) x)` would compute in double against a float -- so the
blast surface is "float32 literal in any mixed expression", not comparison
specifically.

turi keeps the ascription's width, hence the engine divergence.

## Fix direction

Emit a float32-ascribed literal as a single-precision constant -- `7.1f`, or
`(float)7.1` -- so both operands sit in the same width before C's promotion
rules run.  The ascription clearly reaches emit with the right type (the
declared-type path stores it); it is the literal rendering that drops it.

## Why the fixtures never caught it

CLAUDE.md's float-probe rule (lead with `7.1`, never `7.0`) is necessary but
not sufficient here: the wrongness only shows when a float32 literal meets a
float32 VALUE in one expression, and existing float32 coverage prints values
rather than comparing them.  `tests/fixtures/float32-generic-call-result`
now carries a comment pointing at this report and pins its own equality at
2.5 -- exactly representable -- so it tests the generic-result fix rather
than this bug.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- this is not a carrier
crossing (both sides are concrete); it is a literal-width defect adjacent to
the "float crossing the carrier needs a bit reinterpret" note.  Listed in the
open-cells table for visibility since it produces the same symptom class
(silent wrong float).
