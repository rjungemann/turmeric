# `(:: <int> :float)` reinterprets the bit pattern instead of converting

**Severity: medium** -- silently wrong arithmetic, no diagnostic. Found
incidentally while writing an F4 fixture for
[jit-ffi-c2mir-plan](../upcoming/jit-ffi-c2mir-plan.md); unrelated to that
work.

## Repro

```turmeric
(defn main [] : int
  (println (:: 3 :float))   ; prints 1.4822e-323, expected 3
  0)
```

```sh
./build/tur run /tmp/iso.tur
1.4822e-323
```

`1.4822e-323` is `(double)` with the bit pattern `0x0000000000000003` -- the
integer 3 reinterpreted, not converted. It reproduces the same way inside a
struct field read, which is how it was noticed:

```turmeric
(defstruct Mixed [n : int d : float])
(defn mixedfold [m : Mixed] : float
  (+ (:: (.n m) :float) (.d m)))
;; (mixedfold (Mixed 3 0.25)) => 0.25, expected 3.25
```

The denormal contributes nothing to the sum, so the wrong value reads as a
*dropped* term rather than a garbage one -- which is what makes it easy to
miss.

## Why this looks like a bug rather than the intended meaning of `::`

`::` converts in the neighboring case. `tests/fixtures/ascribe-bool-to-numeric-prints`
asserts `(:: true :int32)` prints `1`, i.e. a bool is *converted* to a
numeric, not reinterpreted. Int-to-float taking the other path is an
inconsistency within the same operator, not a documented reinterpret cast --
and Turmeric already spells reinterpretation separately under `unsafe`.

Per the float-testing rule in CLAUDE.md this was probed with a non-integral
literal too: `(:: 3 :float)` and the `Mixed` case above both show the
denormal, so it is the int-to-float direction that is wrong, not a printing
artifact.

## Workaround

`int->float` in `stdlib/math.tur` converts correctly, but it is not
auto-loaded -- a bare use gets `'int->float' lives in stdlib/math.tur and is
not auto-loaded`. So today the choice is `(load "stdlib/math.tur")` or
restructuring to avoid the conversion.

## Fix direction

Not root-caused. The place to look is the ascribe elaboration
(`elab_ascribe`, `src/compiler/elab_types.c`) and whichever emit path it
selects for a numeric target: the bool-to-numeric case clearly reaches a
converting cast, so the int-to-float case is likely falling through to a
carrier-level reinterpretation (the int64 carrier handed straight to a
`double` slot) rather than emitting `(double)`.
