# A method declared `: int` whose instance body produces a float diverges between engines

**Severity:** engine divergence on a shape the language half-forbids --
compiled truncates per the declared type, turi returns the float.
**Status:** OPEN. Pre-existing; surfaced 2026-08-16 while pinning the
method-result float fix (whose in-fixture control for this shape had to be
removed so the fixture stays engine-agreeing).

## Repro -- no inline-C, so run-turi.sh would see it

```turmeric
(defclass Cint [^f] (intify [x [fn :fn]] : int))
(defstruct W [A] (raw :int))
(definstance Cint [W] (intify [x fn] 7.5))
(defn main [] : int
  (let [w (:: (make-struct W 1) (W int))]
    (println (.intify w (fn [x : float] : float x))))
  0)
```

- compiled: `7` (the float body value-converts to the declared `: int` at the
  carrier return)
- turi: `7.5` (the interpreter keeps the body's float value)

## Why this is a semantics question, not a one-line fix

- A plain `defn` declaring `: int` with a float body is **TUR-E0707**, a hard
  error whose message calls the shape "a register-class miscompile, not a
  tolerable carrier bridge". The same shape via a **class-declared** `: int`
  and an instance body producing a float is accepted, and the two engines
  then disagree about what it means.
- The compiled behavior (truncate to the declared type) is load-bearing:
  `tests/fixtures/poly-to-fat-float-roundtrip` pins `7` from a `: int`
  method whose body computes `7.0`. That fixture is invisible to
  `run-turi.sh` (it loads inline-C stdlib, so the harness skips it), which is
  why the divergence has never been red anywhere.
- The 2026-08-16 method-result fix deliberately preserves the compiled
  truncation for this shape: the producer's float bit-cast is keyed on the
  method's DECLARED result kind, so an int-declared method keeps the value
  conversion. This report is about the two engines disagreeing, not about
  that key.

Three consistent resolutions, one of which the language should pick:

1. Extend TUR-E0707 to instance bodies -- an instance whose body type is a
   float while the class-declared result is int becomes the same hard error
   the free-defn shape already is. (Most consistent with E0707's stated
   rationale; would break `poly-to-fat-float-roundtrip` as written.)
2. Declare the compiled truncation the semantics and fix turi to convert.
3. Declare the float-preserving reading the semantics -- but that contradicts
   both the declared `: int` and E0707's register-class argument, and would
   change `poly-to-fat-float-roundtrip`.

## Guide upkeep

Not a carrier-crossing cell -- both representations are "correct" for their
engine's reading of the declared type. Listed for the engine-parity angle.
