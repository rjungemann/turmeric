---
title: Language Readiness for Typed Signal Library
category: Planning
description: A spike-style investigation plan that enumerates the concrete language and stdlib gaps a clean, typed signal-processing library would hit, with one runnable spike per gap. Output is a go / no-go answer for each gap plus filed compiler/stdlib bugs as needed -- not new library code. Gates the tur-signal rebuild and the stdlib/arrow scale-back.
---

# Language Readiness for a Typed Signal Library -- Plan

## Why this plan exists

The previous signal-processing effort (now archived as
[[signal-primitives-expansion-plan]] in `docs/archive/history/`) tried to
keep a working DSP library on top of an ABI that pre-dates the recent
typeclass + fat-closure infrastructure. Every attempt to extend it
required reintroducing the same patterns the language was actively moving
*away* from -- raw `int64_t(*)(int64_t)` casts, `(:: ... :int)` boundary
coercions, untyped pair pointers, untyped state cells. The build-restoration
attempt documented in `docs/reported/signal-spice-broken-build.md`
demonstrated that "fix the spice" is not separable from "decide what the
language can express today."

This plan does **not** propose new library code. It is a sequence of small,
self-contained spikes that answer "does the modern Turmeric type system and
runtime actually support the shape a typed signal library needs?" Each
spike either resolves green (use this in the rebuild), red (file a bug,
gate the rebuild on the fix), or amber (works but with a known sharp edge
the rebuild has to document).

The output is:

1. A green/red/amber verdict per gap, written back into this doc.
2. Compiler / stdlib bug reports filed under `docs/reported/` for each red.
3. A go-signal (or revised gates) for [[tur-signal-rebuild-plan]] and a
   final scope decision for [[stdlib-arrow-scaleback-plan]].

Until every gap is at least amber, neither of those plans starts.

## Non-goals

- Implementing the signal library. That is [[tur-signal-rebuild-plan]].
- Rewriting `stdlib/arrow.tur`. That is [[stdlib-arrow-scaleback-plan]].
- Speculative feature work (sized types, dependent types, refinement). The
  spikes only test what the language *already claims* to support.
- Performance work. A correct-but-slow answer is green for this plan.

## What "the shape a typed signal library needs" means

Concretely, a clean signal library wants:

```
type Time   = :float
type Sample = :float
type Signal A    = Time -> A          ; a function over time
type SF a b      = Signal a -> Signal b
```

with these properties end-to-end:

- `sine`, `square`, etc. produce `SF () Sample` values.
- Combinators (`>>>`, `arrow-first`, `pair-signals`, `mix`) compose them
  without `:int` round-trips.
- A vector of homogeneous SFs (`Vec<SF Sample Sample>`) is constructible
  and walkable, so an effects chain is `(reduce (>>>) input effects)`.
- ADSR state and filter state are typed (e.g. `:ptr<:float>` or an opaque
  `(State F)`), not `:int` handles.
- Sample-typed values (`Sample = :float`) flow through closures and
  collections without bit-casting through `int64`.

Each gap below is one place where that shape may or may not hold today.

## Gaps to investigate

The gaps are listed roughly in dependency order. Earlier reds block later
spikes from being meaningful (e.g., gap G2 is moot if G1 fails).

### G1. `:float`-returning fat closures through composition

**Question**: Can a capturing closure `(fn [t :float] :float ...)` be
returned from a function, passed as an argument, captured by another
closure, and finally invoked -- with the `:float` return preserved end to
end? Specifically through the fat-dispatch path that `^fat` + `TUR_APPLY1`
use.

**Why it matters**: The signal ABI is `Time -> Sample = :float -> :float`.
Every other gap presumes this works.

**Spike**: Create `tests/fixtures/float-fat-closure/`:

- `make-scaler [k :float] : (fn :float :float)` returning a capturing
  closure that multiplies by `k`.
- `compose [^fat f ^fat g] : (fn :float :float)` returning
  `(fn [x] (g (f x)))`.
- `main` invokes `compose (make-scaler 2.0) (make-scaler 3.0)` on `1.0`,
  asserts the result is `6.0` (or whatever the exact representation lands
  at -- tolerance OK).

**Pass**: clean compile, `expected.c` snapshot stable, runs to expected
output under both Debug (ASan) and Release.

**Likely failure modes**: the `stdlib/arrow.tur:91-101` comment flags
"dict field types resolved to `void *` rather than `int64_t`" in
closure-returning typeclass instance methods. The same bug may bite
free functions that return `:float`-valued closures. If so, file
`docs/reported/float-fat-closure-codegen.md` with this exact repro.

---

### G2. Polymorphic `constant` over a type parameter

**Question**: Can a polymorphic constructor

```turmeric
(defn constant [A] [val :A] : (Signal A) (fn [_t] val))
```

be defined when `Signal A` is itself a function-typed alias? Specifically:

- Does the type system accept `Signal A` as a type alias for a function?
- Does monomorphisation produce one specialisation per call-site `A`?
- Does the inner `(fn [_t] val)` capture `val :A` cleanly when `A` is
  `:float`, `:bool`, `:ptr<void>`?

**Why it matters**: Without polymorphic `constant`, every "lift a value to
a signal" call needs a per-type wrapper, which defeats the typing point.

**Spike**: `tests/fixtures/signal-constant-poly/`:

- Declare `Signal` as a type alias if the language supports it; otherwise
  inline the function type.
- Define `constant` polymorphically.
- Instantiate at `Sample` (= `:float`) and at `Bool`; call both at `t=0.5`.
- Assert both round-trip cleanly.

**Pass**: as G1.

**Failure modes**: type aliases over function types may not exist, in
which case the workaround is "spell out the type". File a feature request,
mark amber.

---

### G3. Heterogeneous-by-spelling but homogeneous-by-intent `vec-of`

**Question**: Why does `(vec-of (gain 0.5) (low-pass 0.1) (hard-clip 0.9))`
trip `tur-vec-homog__`? All three are conceptually `SF Sample Sample`, but
the compiler sees their structural return types as distinct (different
captured environments, possibly different `:ptr<void>` vs. captureless).

**Why it matters**: An effects chain wants `Vec<SF Sample Sample>`. If the
homogeneity check is structural over the *closure box layout* rather than
the *function type*, you can't put SFs in a vector at all, and the library
loses one of its core idioms.

**Spike**: `tests/fixtures/sf-vec-of/`:

- Define three SFs with different captured environments but matching
  declared type (`(fn :float :float)`).
- Try `(vec-of sf1 sf2 sf3)` directly.
- Try `(vec-of (:: sf1 :ptr<void>) ...)` and observe whether the result is
  walkable.

**Pass**: any spelling that puts three distinct closures of declared type
`(fn :float :float)` into one vec without manual `:int` boxing.

**Failure modes**:

- If `tur-vec-homog__` insists on identical concrete C types per element,
  file `docs/reported/vec-of-closure-homogeneity.md` and propose loosening
  the check to "all elements unify to the same declared function type".
- If even with manual `:ptr<void>` boxing the vec-walker can't dispatch
  cleanly, that's a deeper gap. Document either way.

---

### G4. Typed `Pair[A B]` through closure-returning code

**Question**: Can a function `pair-signals` whose declared return type is
`(Signal (Pair A B))` actually build a `Pair[A B]` *inside* an
`(fn [t] ...)` body via `make-struct Pair` and have callers consume it via
`pair-fst` / `pair-snd` without ever reinterpreting the value as `:int`?

**Why it matters**: The old library walked pairs as `:int` pointers to
`{int64_t e1; int64_t e2;}` C structs and added local
`__pair-fst-int`/`__pair-snd-int` shims because `(first p)` (the Arrow
typeclass method) is not what you want here. Typed `Pair[A B]` from
`stdlib/pair.tur` is the correct abstraction; G4 verifies it survives the
closure boundary.

**Spike**: `tests/fixtures/pair-signals-typed/`:

- `pair-signals [^fat sa ^fat sb] : (Time -> Pair Float Float)`.
- Construct via `make-struct Pair (sa t) (sb t)` inside an `(fn [t] ...)`.
- Caller invokes the returned signal at `t=0.0`, then
  `(pair-fst result)` and `(pair-snd result)` -- both typed `:float`.

**Pass**: as G1.

**Failure modes**: `make-struct Pair` may not be allowed inside a closure
body, or the typed `Pair[Float Float]` may not be a valid return type for
a closure. Either is a real compiler/stdlib bug to file.

---

### G5. Typed mutable state cells (`State F` / `:ptr<:float>`)

**Question**: Can a filter's mutable state -- currently `int64_t` carrying
a `double *` allocated via inline-C `calloc` -- be typed as
`:ptr<:float>` (or wrapped in an opaque `(defopaque State :ptr<:float>)`)
and read/written from inline-C without going through `(intptr_t)` casts?

**Why it matters**: Every DSP primitive in the previous library hid state
behind `:int` handles. Typing them removes a whole class of
"`memcpy(&x, &sig_val, 8)`" boundary noise and prevents accidental
state-confusion across filter instances.

**Spike**: `tests/fixtures/typed-state-cell/`:

- `(defn alloc-state [] :ptr<:float> ```c return calloc(1, sizeof(double)); ```)`
- A consumer that reads/writes `*state` via inline-C with the typed
  parameter, not `(intptr_t)` cast.
- Verify the `expected.c` is free of `(intptr_t)` for the state pointer.

**Pass**: clean compile + the generated C uses `double *` directly, not
`int64_t` with a cast inside the body.

**Failure modes**: inline-C may accept `:ptr<:float>` as the parameter but
still mint `int64_t state` in the wrapper, forcing the cast back in.
Document the gap; this becomes a stdlib/codegen improvement plan rather
than a hard blocker.

---

### G6. Vec[Closure] walking via `vec-get` typing

**Question**: When `Vec<SF Sample Sample>` is constructed (assuming G3
passes), does `(vec-get v i)` return a value usable directly as a fat
closure -- i.e. typed as the SF type, not as `:int` that has to be
reinterpreted?

**Why it matters**: `stdlib/vec.tur:65-74` defines `vec-get` as
`[A] [v :int i :int] :A`. So the return type follows `A`. If `A` is an SF
function type, can the caller invoke the result with `^fat` dispatch
without an explicit `:: :ptr<void>` cast at every read?

**Spike**: `tests/fixtures/vec-get-closure/`:

- A vec of `(fn :int :int)` closures.
- `(let [f (vec-get v 0)] (f 42))` -- assert returns expected value.

**Pass**: as G1.

**Failure modes**: closure call on a `vec-get` result may need an
intervening `^fat` shim or explicit cast. Document the workaround;
likely amber.

---

### G7. `>>>` and `arrow-first` typed end-to-end

**Question**: Given G1 + G3 + G6, does the existing `>>>` (`stdlib/arrow.tur:131-132`)
compose two SFs cleanly when both arguments are typed `:ptr<void>` /
fat-closure, and does the returned closure have a usable declared type?

**Why it matters**: This is the integration test. Even if every sub-spike
passes, the composition site is where the typing usually frays.

**Spike**: `tests/fixtures/sf-compose-typed/`:

- Two `(fn :float :float)` closures.
- `(>>> sf1 sf2)`; call the result at `1.0`; assert correct value.
- Verify the result can be stored in `Vec<(fn :float :float)>` and read
  back.

**Pass**: as G1; bonus pass if `(>>> sf1 sf2)` infers a result type usable
without manual annotation at the caller.

**Failure modes**: most likely fine since `arrow-capturing-closure` and
`stdlib-arrow` fixtures already exercise the bare-function path. If this
fails, something deeper changed between those fixtures and the typed
shape; investigate in isolation.

---

### G8. `tur-signal-rebuild` ABI sanity end-to-end

**Question**: Building on G1-G7, can a 30-line "two oscillators summed and
filtered" toy actually compile and run, with all types declared, no
`:int` round-trips, no `__arrow_call1` shim?

**Why it matters**: This is the smoke test that proves the previous
report's "stop spice-side work" recommendation has actually been
unblocked. If G1-G7 are all green but G8 fails, the rebuild plan's gates
are wrong and need to expand.

**Spike**: `tests/fixtures/typed-signal-smoke/`:

- Two sine SFs, `(>>> (par-comp sine1 sine2) sum)` style.
- `(low-pass alpha)` returning a typed SF.
- `main` evaluates the chain at 8 sample positions, prints them.
- Expected output matches a hand-computed reference.

**Pass**: clean Debug + Release builds, leak-clean under ASan/LSan, output
matches.

**Failure modes**: anything that surfaces here that isn't already
captured by G1-G7 gets backported into this plan as a new gap.

## Phasing

These are spikes, not a phased rollout. Run them in dependency order:

1. **Sprint 1**: G1, G5 (independent, both verify the basic ABI claims).
2. **Sprint 2**: G2, G3, G6 (depend on G1).
3. **Sprint 3**: G4, G7 (depend on G2 + G3).
4. **Sprint 4**: G8 (depends on all of the above).

If any sprint has a red, decide whether to: (a) fix the underlying issue
in this repo and continue, (b) document the workaround and continue with
amber, or (c) stop and re-scope. (c) is the explicit option -- the whole
point of this plan is that pushing through gaps creates the warts.

A spike is "done" when:

- The fixture under `tests/fixtures/<spike-name>/` lives in tree.
- It runs cleanly under `bash tests/run.sh`.
- This doc has a verdict line filled in for that gap (see template
  below).

## Verdict template

When each spike resolves, append a status block at the end of its section:

```
**Status**: green | red | amber.
**Fixture**: tests/fixtures/<name>/
**Notes** (red/amber only): one paragraph.
**Filed report** (red only): docs/reported/<slug>.md
```

## Acceptance for this plan

This plan is done when every gap G1-G8 has a verdict block filled in. At
that point the output gets summarised at the top of the doc, and
[[tur-signal-rebuild-plan]] picks up.

## Open questions

- **Do we need a real `Signal A` type alias?** Or is spelling
  `(Time -> A)` everywhere acceptable? G2's findings drive this.
- **Should the homogeneity check loosen, or should signal use a typed
  `List<SF a b>` instead of `Vec<SF a b>`?** Wait for G3.
- **Is the `>>>` mangling collision with `<<<` worth re-litigating?**
  Currently `stdlib/arrow.tur:231-233` notes both mangle to `___`; only
  `>>>` exists. Defer unless a real ergonomic case shows up.

## Cross-references

- Supersedes `docs/archive/history/signal-primitives-expansion-plan.md` as
  the prerequisite for any signal-spice work.
- Feeds [[tur-signal-rebuild-plan]] and [[stdlib-arrow-scaleback-plan]].
- Reports findings at `docs/reported/signal-spice-broken-build.md`
  (the trigger for this plan) and any new `docs/reported/*` filed during
  spikes.
