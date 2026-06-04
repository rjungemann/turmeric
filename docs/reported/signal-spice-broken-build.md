---
title: tur-signal spice does not currently compile; Phase 0 plan understates scope
category: Reported
severity: high
description: The signal spice's source files reference `__arrow_call1` (removed from stdlib) plus several un-imported stdlib symbols, so `tur check` fails on `core.tur` and `synth.tur` before any :float migration work begins. The plan in `docs/upcoming/signal-primitives-expansion-plan.md` assumes a "half-done :float migration on otherwise working code" baseline that no longer matches reality.
---

# `tur-signal` spice: broken build + closure-ABI latent bug

## Summary

One-line: the sibling spice `../turmeric-spices/spices/signal/` no longer
typechecks against the current `tur` (`./build/tur`). The
signal-primitives-expansion plan's Phase 0 ("finish the `:float` sample
migration") therefore cannot land on top of a working baseline; it has to be
preceded by a build-restoration pass that the plan does not currently
acknowledge.

Severity: **high for the plan, medium for the project.** The spice is unusable
as published, but nothing in the main turmeric repo depends on it at build
time -- only the planning doc does. Anyone following the plan literally will
discover the breakage after the first `tur check` and have to stop.

## Observed vs. expected

### Expected (from `docs/upcoming/signal-primitives-expansion-plan.md`)

> `signal/synth.tur:7` notes: "All sample values are now native `:float`."
> but the existing C helpers still `memcpy` between `int64_t` and `double`,
> and every SF body bit-casts at its boundaries [...] The migration is
> half-done.

This frames Phase 0 as a stylistic cleanup pass on otherwise functional code.

### Observed

`./build/tur check` against each `src/signal/*.tur` file produces real errors,
not just bit-cast noise:

```
$ ./build/tur check ../turmeric-spices/spices/signal/src/signal/core.tur
core.tur:81:20: error: unknown function or operator '__arrow_call1'
  (fn [t] (pair (__arrow_call1 sa t) (__arrow_call1 sb t)))))

$ ./build/tur check ../turmeric-spices/spices/signal/src/signal/synth.tur
synth.tur:399: error: unknown function or operator 'vec-length'
synth.tur:414: error: unknown function or operator 'gain'
synth.tur:436: error: unknown function or operator 'vec-length'
... and more
```

`dsp.tur` and `envelope.tur` check cleanly in isolation; the breakage is in
`core.tur` (uses removed stdlib symbol) and `synth.tur` (missing imports for
`stdlib/vec`, `signal/dsp`, `stdlib/arrow`).

## Root cause

### 1. `__arrow_call1` was removed from stdlib

`stdlib/arrow.tur:81-90` documents the removal:

> Every helper then dispatches uniformly through the fat protocol
> (TUR_APPLY1, which reads the thunk from slot 0 and passes the box as the
> environment), or by calling the closure directly in Turmeric (a
> `:ptr<void>` direct call already fat-dispatches). This replaces the former
> `__arrow_call1`/`__arrow_call2` thin int64 casts, which segfaulted on
> capturing closures. See
> docs/upcoming/closure-representation-unification-plan.md.

`grep "defn __arrow_call1" stdlib/ ../turmeric-spices/` finds zero hits --
the symbol was removed wholesale, not renamed. The signal spice still calls
it in:

- `src/signal/core.tur:81` (`pair-signals`)
- `src/signal/core.tur:51` (`sample`)
- `src/signal/core.tur:66` (`map-signal`)
- `src/signal/synth.tur` -- 17 call sites in `voice`, `voice-sf`, `poly-synth`,
  `hard-clip`, `wavetable-voice`, `ks-voice`, plus the now-stubbed `fm-voice`.
- `tests/signal/arrow_tests.tur` -- 50+ call sites.

### 2. `synth.tur` is missing top-level imports

`synth.tur` has no `(import ...)` or `(defmodule ...)` form. It relies on
`gain`, `low-pass`, `hard-clip`, `>>>`, `sine`, `sawtooth`, `vec-length`,
`vec-get`, `vec-create`, `vec-set!`, `vec-of`, `pair`, `first`, `second`,
`adsr-fixed`, `ADSRParams`, `constant` -- all of which live in either
`signal/dsp`, `signal/core`, `signal/envelope`, `stdlib/vec`, `stdlib/pair`,
or `stdlib/arrow`. None are imported.

The per-file build path described in the project CLAUDE.md
("`tur check` walks up looking for `build.tur`, adds spice's `src/` plus
each `:spices` dep's `src/` to the include path") finds the sibling
spice modules but does not auto-resolve their *symbols* -- the `synth.tur`
file still needs explicit `(import signal/dsp)` etc. at the top.

### 3. Latent closure-ABI bug in `dsp.tur` inline-C

Even ignoring the Phase 0 :float migration, the existing inline-C in
`low-pass`, `high-pass`, `gain`, `mix`, and `add` calls captured closures via:

```c
int64_t sig_val = ((int64_t(*)(int64_t))(intptr_t)sv)(t);
```

per `dsp.tur:112`, `:140`, `:169`, `:193`, `:213`. `sv` is bound by `(let [sv
sig] ...)` inside a `(fn [sig] (let [...] (fn [t] ...)))` -- so the inner
closure captures `sv`, making `sv` a **fat closure** (heap box whose slot 0
holds the thunk) in the post-closure-unification representation, not a bare
captureless C function pointer.

The stdlib comment in `arrow.tur:88` is explicit: this exact cast pattern
"segfaulted on capturing closures." So `low-pass`/`high-pass`/`gain`/`mix`/
`add` -- the supposedly-working DSP primitives the plan inherits -- contain
a latent crash that will be hit the moment a test exercises a stateful
filter under the new dispatch convention.

The Phase 0 migration touches exactly these inline-C bodies. Doing the
migration without simultaneously switching to TUR_APPLY1 / direct-closure
dispatch will preserve the bug under a new float ABI, which is worse than
the status quo (today: source-level error, easy to spot; tomorrow: runtime
segfault under a load test).

### 4. Closure-return-type ABI question for Phase 0 itself

Phase 0 specifies `__signal_call1` returns `:float`, not `:int`. The current
body (`core.tur:112-115`) is a raw function-pointer cast:

```c
return ((int64_t(*)(int64_t))(intptr_t)sig)(t);
```

The Phase 0 version would presumably become:

```c
return ((double(*)(int64_t))(intptr_t)sig)(t);
```

For this to work, every Turmeric closure that gets passed as a Signal must
codegen-emit a C function with signature `double(int64_t)` (or, under fat
dispatch, a thunk with the matching return type). I have not verified that
the Turmeric closure codegen + fat-dispatch path supports `:float`-returning
closures end-to-end. If it does not, Phase 0 is blocked on a compiler change
in this repo, not just a spice edit.

## Minimal repro

```sh
# from turmeric repo root, with ./build/tur present
./build/tur check ../turmeric-spices/spices/signal/src/signal/core.tur
# expected: clean
# observed: unknown function or operator '__arrow_call1'

./build/tur check ../turmeric-spices/spices/signal/src/signal/synth.tur
# observed: unknown function or operator 'vec-length' / 'gain' / etc.
```

## Proposed fix direction

Phase 0 of `signal-primitives-expansion-plan.md` should be split:

- **Phase 0a -- restore the build.** Either:
  - (a) reintroduce `__arrow_call1` as a stdlib-side shim that does the right
    fat-dispatch (TUR_APPLY1) and re-exports for the spice, or
  - (b) rewrite every `__arrow_call1` call site in the spice to use direct
    Turmeric closure invocation (`(sv t)` instead of `(__arrow_call1 sv t)`)
    plus the `^fat` parameter annotation where the closure is consumed.
  - **Plus**: add the missing `(import ...)` forms to `synth.tur` and
    `tests/signal/arrow_tests.tur`.
  - Validation: `./build/tur check` clean on every `src/signal/*.tur` and
    every `tests/signal/*.tur`, plus the existing test suite passes on the
    current int64-bit-cast sample ABI.
- **Phase 0b -- closure-ABI cleanup.** Replace the raw
  `int64_t(*)(int64_t)` inline-C casts in `dsp.tur` with fat-dispatch helpers
  (TUR_APPLY1 or a typed Turmeric trampoline). Required regardless of the
  :float migration; the existing pattern is documented-broken.
- **Phase 0c -- :float sample migration** (the originally-named Phase 0).
  Land after 0a + 0b. First confirm the Turmeric closure codegen supports
  `:float`-returning closures through fat dispatch; if not, file a separate
  compiler-side bug and gate 0c on it.

The acceptance gate in the plan ("`grep -n "memcpy(&" src/signal/`"
returns nothing) stays, but only kicks in after 0a/0b are green.

## Validation of a fix

A fix is good if:

1. `./build/tur check` passes on every file under
   `../turmeric-spices/spices/signal/src/` and `tests/signal/`.
2. Whatever test runner the spice uses (`tur run` against
   `tests/signal/arrow_tests.tur`?) passes with zero failures.
3. The DSP filter tests that exercise `low-pass`/`high-pass`/`gain` actually
   run a multi-sample sequence (impulse response or similar) -- the current
   tests appear to call them but I have not confirmed they crash under
   the fat-dispatch repro. If they do not crash today, document *why* (e.g.
   the codegen still emits captureless closures for these arities) so the
   Phase 0b scope can be sized correctly.
