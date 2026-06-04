---
title: Arrows and Signal Processing
category: Language Features
description: Bare-function arrow combinators and building DSP signal graphs with stdlib/arrow.tur and stdlib/signal/
---

# Arrows and Signal Processing

Turmeric's `stdlib/arrow.tur` provides a small set of **bare-function arrow
combinators** over the `(->)` function arrow. They describe composable
computations that take an input and produce an output -- `arr`, `>>>`,
`arrow-first`, `arrow-second`, `par-comp`, `arrow-split`, `arrow-const`, and
`arrow-dup`.

The most immediate use case in Turmeric is **signal processing**: the
`stdlib/signal/` libraries build on these combinators to create composable
DSP graphs from pure building blocks.

## Why bare functions, not typeclass dispatch

Earlier drafts declared a full Haskell-style Arrow typeclass hierarchy
(`Arrow`, `ArrowZero`, `ArrowPlus`, `ArrowChoice`, `ArrowLoop`, `ArrowApply`),
but those declarations were never backed by working `definstance` forms --
closure-returning instance methods tripped a codegen bug and several stub
bodies required language features (`Either`/`Left`/`Right`, full Tuple
feedback) that do not exist yet. The scaleback in
[`docs/upcoming/stdlib-arrow-scaleback-plan.md`](../upcoming/stdlib-arrow-scaleback-plan.md)
removed the dead typeclass scaffolding and kept only the bare functions, which
are exercised by `tests/fixtures/stdlib-arrow` and
`tests/fixtures/arrow-capturing-closure`. If and when the typeclass codegen
gap closes, the hierarchy can be reintroduced through its own plan; until
then, write `(>>> f g)` rather than reaching for a typeclass method.

For functions, `arr` is the identity, and `>>>` is left-to-right composition.

## Function Arrow Basics

```turmeric
(import stdlib/arrow.tur)

;; arr: lift a function (identity for the function Arrow)
(let [add1   (arr (fn [x] (+ x 1)))
      double (arr (fn [x] (* x 2)))]

  ;; >>> : compose left-to-right: add1 first, then double
  (let [pipeline (>>> add1 double)]
    (println (pipeline 5))))   ; => 12
```

```sweet-exp
import stdlib/arrow.tur

let [add1   arr(fn([x] +(x 1)))
     double arr(fn([x] *(x 2)))]

  let [pipeline >>>(add1 double)]
    println pipeline(5)        ; => 12
```

The dataflow reads left-to-right: input enters `add1`, the result flows into
`double`, and the final value exits.

## Arrow Laws

Any correct Arrow instance must satisfy:

```
arr id >>> f        = f                      ; identity
arr (g . f)         = arr f >>> arr g        ; composition
(f >>> g) >>> h     = f >>> (g >>> h)        ; associativity
first (arr f)       = arr (first f)          ; naturality
first (f >>> g)     = first f >>> first g    ; product distribution
```

Turmeric does not enforce these at compile time, but violating them produces
pipelines with unexpected behaviour.

## Product Combinators

`first` and `second` let you route a branch of a `Pair` through an arrow while
passing the other branch unchanged.

```turmeric
(import stdlib/arrow.tur)

(let [add1       (arr (fn [x] (+ x 1)))
      first-add1 (arrow-first add1)    ; apply add1 to fst, pass snd through
      p          (Pair 5 10)]
  (println (first-add1 p)))            ; => Pair(6, 10)
```

```sweet-exp
import stdlib/arrow.tur

let [add1       arr(fn([x] +(x 1)))
     first-add1 arrow-first(add1)
     p          Pair(5 10)]
  println first-add1(p)                ; => Pair(6, 10)
```

`arrow-first` and `arrow-second` are the plain-function helpers exported from
`stdlib/arrow.tur`. Reverse composition (`<<<`) is intentionally not exported
-- both `<<<` and `>>>` mangle to the same C identifier (`___`), so they
cannot coexist as free `defn`s. Write `(>>> f g)` with the arguments in the
order you want them applied.

## Additional Combinators

`stdlib/arrow.tur` exports several convenience combinators:

### `par-comp` (parallel composition)

Apply `f` to the first component of a `Pair` and `g` to the second simultaneously.

```turmeric
(let [both (par-comp 0 0 0 0 (fn [x] (+ x 1)) (fn [x] (* x 2)))]
  (both (Pair 3 4)))   ; => Pair(4, 8)
```

```sweet-exp
let [both par-comp(0 0 0 0 fn([x] {x + 1}) fn([x] {x * 2}))]
  both Pair(3 4)       ; => Pair(4, 8)
```

Note: the first four `0` arguments are type-level placeholders (the type
variables `a b c d`); they are ignored at runtime.

### `arrow-split` (fanout)

Duplicate a value and apply two functions, collecting results in a `Pair`.

```turmeric
(let [split (arrow-split 0 0 0 (fn [x] (+ x 1)) (fn [x] (* x 2)))]
  (split 3))   ; => Pair(4, 6)
```

```sweet-exp
let [split arrow-split(0 0 0 fn([x] {x + 1}) fn([x] {x * 2}))]
  split 3      ; => Pair(4, 6)
```

### `arrow-const` and `arrow-dup`

```turmeric
((arrow-const 42) 999)        ; => 42  (ignores input)
(tuple2-1st (arrow-dup 7))    ; => 7   (Tuple2(7, 7))
```

```sweet-exp
arrow-const(42)(999)          ; => 42
tuple2-1st arrow-dup(7)       ; => 7
```

---

## Signals and Signal Functions

`stdlib/signal/core.tur` introduces the Signal abstraction:

```
Signal a  =  Time -> a
SF a b    =  Signal a -> Signal b
```

A **Signal** is just a function from time to a value. A **Signal Function** (SF)
transforms one signal into another. Because SF is itself a function `(Signal a ->
Signal b)`, the bare-function arrow combinators apply without any extra
machinery: `arr`, `>>>`, `arrow-first`, and `arrow-second` all work on SFs
out of the box.

### Core signal constructors

```turmeric
(import stdlib/signal/core.tur)

;; constant: ignore time, always return the same value
(let [dc (constant 5.0)]
  (dc 0.0)    ; => 5.0
  (dc 99.0))  ; => 5.0

;; time-signal: identity -- returns the time at each sample
(time-signal 2.5)   ; => 2.5

;; sample: evaluate a signal at a point in time
(sample (constant 3.0) 1.0)   ; => 3.0

;; map-signal: lift a function to pointwise operation over a signal
(let [louder (map-signal (fn [x] (* x 2.0)) (constant 0.5))]
  (louder 0.0))   ; => 1.0
```

```sweet-exp
import stdlib/signal/core.tur

let [dc constant(5.0)]
  dc(0.0)    ; => 5.0
  dc(99.0)   ; => 5.0

time-signal 2.5           ; => 2.5
sample(constant(3.0) 1.0) ; => 3.0

let [louder map-signal(fn([x] {x * 2.0}) constant(0.5))]
  louder(0.0)             ; => 1.0
```

### Combining signals

`pair-signals` zips two signals into a product signal, needed by combinators
like `mix` and `add` that take stereo or multi-channel input:

```turmeric
(let [prod (pair-signals (constant 1.0) (constant 2.0))]
  (prod 0.0))   ; => Pair(1.0, 2.0)
```

```sweet-exp
let [prod pair-signals(constant(1.0) constant(2.0))]
  prod(0.0)     ; => Pair(1.0, 2.0)
```

---

## DSP Primitives

`stdlib/signal/dsp.tur` provides ready-made oscillators, filters, and amplitude
operations, each as a Signal Function.

### Oscillators

```turmeric
(import stdlib/signal/core.tur)
(import stdlib/signal/dsp.tur)

(let [dummy  (constant ())
      ;; sine oscillator: freq Hz, initial phase in radians
      a440   (sine 440.0 0.0)
      ;; square wave: freq, duty cycle [0,1]
      sq     (square 220.0 0.5)
      ;; sawtooth wave: freq
      saw    (sawtooth 110.0)
      ;; triangle wave (stdlib/signal/dsp.tur -- if available)
      tri    (triangle 55.0)]

  ;; An oscillator SF is called with a (usually dummy) input signal
  ;; and returns the output signal.
  ((a440 dummy) 0.001))
```

```sweet-exp
import stdlib/signal/core.tur
import stdlib/signal/dsp.tur

let [dummy  constant(())
     a440   sine(440.0 0.0)
     sq     square(220.0 0.5)
     saw    sawtooth(110.0)
     tri    triangle(55.0)]

  a440(dummy)(0.001)
```

The oscillators ignore their input signal; the `dummy` argument satisfies the
SF interface so they compose uniformly with filters and other SFs.

### Filters

```turmeric
;; low-pass: exponential moving average.
;; alpha in (0,1) -- lower alpha = more smoothing (shorter cutoff)
(let [lp  (low-pass 0.1)
      sig (constant 1.0)
      out (lp sig)]
  (out 0.0)    ; starts at 0 (filter state is zero-initialised)
  (out 0.001))

;; high-pass: subtract the low-passed signal from the input
(let [hp  (high-pass 0.1)
      out (hp sig)]
  (out 0.001))
```

```sweet-exp
let [lp  low-pass(0.1)
     sig constant(1.0)
     out lp(sig)]
  out(0.0)
  out(0.001)

let [hp  high-pass(0.1)
     out hp(sig)]
  out(0.001)
```

### Amplitude operations

```turmeric
(let [sig    ((sine 440.0 0.0) (constant ()))

      ;; gain: scale every sample by a constant
      gained ((gain 0.5) sig)

      ;; mix: weighted blend of a pair signal
      ;; alpha=0 -> all first, alpha=1 -> all second
      blended ((mix 0.3) (pair-signals sig (constant 0.0)))

      ;; add: sample-wise sum of a pair signal
      summed  ((add) (pair-signals sig sig))]
  ...)
```

```sweet-exp
let [sig     sine(440.0 0.0)(constant(()))

     gained  gain(0.5)(sig)
     blended mix(0.3)(pair-signals(sig constant(0.0)))
     summed  add()(pair-signals(sig sig))]
  ...
```

---

## Building a DSP Signal Graph

Because SFs are plain functions, `>>>` wires them into graphs. Read each `>>>`
as "then": input flows left-to-right through each stage.

```turmeric
(import stdlib/signal/core.tur)
(import stdlib/signal/dsp.tur)

(let [dummy (constant ())
      input ((sine 440.0 0.0) dummy)

      ;; Signal graph:
      ;;   sine 440 Hz
      ;;     -> gain 0.5
      ;;     -> low-pass (alpha 0.1)
      ;;     -> DC offset +0.1
      ;;     -> clip [-0.8, 0.8]
      chain (>>> (>>> (>>> (gain 0.5)
                           (low-pass 0.1))
                      (offset 0.1))
                 (clip -0.8 0.8))

      output (chain input)]

  (output 0.0)
  (output 0.001)
  (output 0.002))
```

```sweet-exp
import stdlib/signal/core.tur
import stdlib/signal/dsp.tur

let [dummy  constant(())
     input  sine(440.0 0.0)(dummy)

     chain  >>>(>>>(>>>(gain(0.5) low-pass(0.1))
                        offset(0.1))
                   clip(-0.8 0.8))

     output chain(input)]

  output(0.0)
  output(0.001)
  output(0.002)
```

### Parallel paths with `pair-signals`

Route a signal down two parallel branches and mix the results:

```turmeric
(let [dummy     (constant ())
      raw       ((sine 440.0 0.0) dummy)

      ;; Branch A: full signal
      branch-a  raw

      ;; Branch B: low-passed version
      branch-b  ((low-pass 0.05) raw)

      ;; Mix 50/50
      output    ((mix 0.5) (pair-signals branch-a branch-b))]

  (output 0.001))
```

```sweet-exp
let [dummy    constant(())
     raw      sine(440.0 0.0)(dummy)

     branch-a raw
     branch-b low-pass(0.05)(raw)

     output   mix(0.5)(pair-signals(branch-a branch-b))]

  output(0.001)
```

### Multi-oscillator mix

```turmeric
(let [dummy  (constant ())
      a440   ((sine 440.0 0.0) dummy)   ; root
      e660   ((sine 660.0 0.0) dummy)   ; perfect fifth
      summed ((add) (pair-signals a440 e660))
      out    ((gain 0.5) summed)]       ; normalise

  (out 0.001))
```

```sweet-exp
let [dummy  constant(())
     a440   sine(440.0 0.0)(dummy)
     e660   sine(660.0 0.0)(dummy)
     summed add()(pair-signals(a440 e660))
     out    gain(0.5)(summed)]

  out(0.001)
```

---

## Extended Arrow Typeclasses

Earlier drafts declared `ArrowZero`, `ArrowPlus`, `ArrowChoice`, `ArrowLoop`,
and `ArrowApply` in `stdlib/arrow.tur`. Those declarations were removed by
the scaleback in
[`docs/upcoming/stdlib-arrow-scaleback-plan.md`](../upcoming/stdlib-arrow-scaleback-plan.md)
because no instances ever backed them (the closure-returning instance methods
tripped a codegen bug, and several stub bodies needed `Either`/`Left`/`Right`
sum types that do not exist yet). When that codegen gap closes and the sum
types land, reintroducing the hierarchy is a follow-up plan, not something
the current stdlib pretends to support.

---

## Examples

The `examples/signal-processing/` directory contains a three-step tutorial
that you can run directly:

```sh
tur run examples/signal-processing/01_basics.tur    # Arrow fundamentals
tur run examples/signal-processing/02_signals.tur   # Signals and SFs
tur run examples/signal-processing/03_dsp.tur       # DSP primitives
```

**`01_basics.tur`** -- covers `arr`, `>>>`, arrow laws, and
`arrow-first`/`arrow-second` using plain integer functions.

**`02_signals.tur`** -- introduces the `Signal` and `SF` types, `constant`,
`time-signal`, `pair-signals`, and stateful SFs via `state-sf`.

**`03_dsp.tur`** -- demonstrates oscillators (`sine`, `square`, `sawtooth`,
`triangle`), processors (`gain`, `offset`, `invert`, `abs-sf`), mixing
(`add`, `mix`, `multiply`), filters (`low-pass`, `high-pass`), and a
complete multi-stage processing chain.

## Quick Reference

```
stdlib/arrow.tur          -- arr, >>>, arrow-first, arrow-second,
                             par-comp, arrow-split, arrow-const, arrow-dup
stdlib/signal/core.tur    -- constant, time-signal, sample, map-signal,
                             pair-signals, left-signal, right-signal
stdlib/signal/dsp.tur     -- sine, square, sawtooth, triangle,
                             low-pass, high-pass,
                             gain, mix, add, offset, clip, invert
```

## See Also

- **[hkt-guide.md](hkt-guide.md)** -- higher-kinded types; the `^arr` kind used by the Arrow typeclass
- **[effects-vs-monads.md](effects-vs-monads.md)** -- design rationale: why Turmeric uses effects rather than monad transformers for sequencing
- **[generators-guide.md](generators-guide.md)** -- lazy sequences; a complementary pull-based abstraction
