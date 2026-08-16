# A method declared `: int` whose instance body produces a float diverges between engines

**Severity:** engine divergence on a shape the language half-forbids --
compiled truncates per the declared type, turi returns the float.
**Status: RESOLVED 2026-08-16, same day** -- by resolution 1 (extend
TUR-E0707 to instance bodies), which the measurement made cheap after it
first exposed a bigger finding than the divergence itself.

**What the investigation found.**  The tolerance's stated justification --
"the per-instance emit path resolves a non-float-declared / float-body
method to its real register class" -- is FALSE in the tree: the emitted C
is `static int64_t ... { return 7.5; }`, a destructive value conversion.
And the shape is ambiguous by construction: stdlib `Clone`'s float32
instance wanted BITS through the `: int` slot (an identity clone), while
the poly-to-fat fixtures wanted the VALUE conversion -- two intents, one
spelling, nothing structural to tell them apart.  Worse, the bits intent
never worked compiled: **`(.clone (:: 7.1 float32))` returned `7`** --
a silent stdlib wrong answer (turi: 7.1), because
`(defclass Clone [a] (clone [x] : int))` was the lazy `:int` slot CLAUDE.md
forbids.

**What landed, in order, each measured:**

1. **stdlib `Clone` got its honest signature** --
   `(defclass Clone [a] (clone [x : a] : a))` (both declarations,
   `typeclass.tur` and the auto-loaded `typeclass-clone.tur` stub).  The
   float32 clone now round-trips 7.1.  Corpus sweep of would-reject sites:
   **102 -> 3** -- the 102 were all the one stdlib instance elaborating per
   program, and the 3 survivors are the poly-to-fat family, whose `: int`
   was incidental to their point (the typed shim ABI).
2. **Those 3 fixtures migrated to `: float`** -- their expected stdout is
   unchanged (`%.7g` of 7.0 prints `7`), and their float-declared results
   now route through the same-day method-result fix.
3. **The register-class check went symmetric for every return class**
   (`return_position_conflict`, elab_core.c) -- a float body under a fixed
   non-float instance slot is now the same TUR-E0707 the equivalent defn
   has always been, in both engines (elaboration is shared, so the
   divergence is resolved by elimination).  The cstr-under-`: int` carrier
   bridge stays accepted -- pointer bits genuinely ride the carrier
   losslessly, so that half of the old blessing survives on its merits
   (positive control: `instance-method-return-committed-ok`).

Pinned by `errors/instance-method-return-float-under-int-slot`; five
fixtures that locally re-declared the old `Clone` signature were migrated
(the capture-precision one merges with the new stdlib signature, since the
cloneable-capture machinery keys on the class NAME; the two
parametric-clone ones and the aggregate-vs-scalar negative renamed their
local class to `CloneI` to keep exercising the carrier-slot shape).  Suite
2602/0, fuzzer seeds 9601/9602 clean, one snapshot regenerated
(`map-multiword-struct-value` -- Clone instances now declare their real
return types).

Originally filed as OPEN; surfaced while pinning the method-result float fix
(whose in-fixture control for this shape had to be removed so the fixture
stayed engine-agreeing).

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
