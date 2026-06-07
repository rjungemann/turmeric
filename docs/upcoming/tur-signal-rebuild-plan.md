---
title: tur-signal Rebuild Plan
category: Planning
description: Rebuild the tur-signal spice from scratch on the modern typed + fat-closure infrastructure. Supersedes signal-primitives-expansion-plan (archived) and explicitly does not carry forward the int64-bit-cast sample ABI, raw __arrow_call1 thin casts, or untyped pair handles. Gated on the spikes in language-readiness-for-typed-signal-plan; nothing under spices/signal/ starts until those spikes are green or amber.
---

# `tur-signal` Rebuild -- Plan

## Prereq audit (2026-06-05)

Audit run against `main` at 945cabc6 with a fresh `build/tur`. Verdict:
**all hard prerequisites hold; the rebuild is unblocked.**

- **Readiness fixtures** (G1-G8) -- all 9 probe fixtures present and
  passing on a fresh Debug build:
  `float-fat-closure`, `fat-closure-float-compose`, `sf-vec-of`,
  `pair-signals-typed`, `typed-state-cell`, `vec-get-closure`,
  `sf-compose-typed`, `typed-signal-smoke`, and the
  `errors/poly-closure-result-tyvar-float` hard-error fixture
  (`summary: 9 passed, 0 failed`). The G1-G8 verdict block in the
  archived [[language-readiness-for-typed-signal-plan]] still reflects
  reality.
- **Arrow composition (updated 2026-06-06)** -- `stdlib/arrow.tur`'s
  `>>>` is now the polymorphic typed compositor (Stage E,
  [[poly-closure-result-specialization-plan]] done). `compose-float`
  deleted. Phase 5 `effects-chain` should use `>>>` directly;
  `compose-float` callers need to be migrated. G7 amber cleared.
- **Stale background** -- the "Background" section below says the
  `../turmeric-spices/spices/signal/` tree no longer exists. **This is
  stale.** The spice was revived in `turmeric-spices` (commits
  `b9c72b5` "Revive signal spice", `5220e61` "signal spice"). The
  current layout is:
  - `src/signal/core.tur` (112 lines) -- check-clean
  - `src/signal/dsp.tur` (165 lines) -- check-clean
  - `src/signal/envelope.tur` (77 lines) -- check-clean
  - `src/signal/synth.tur` (390 lines) -- check exit 0 with a
    `>>>`-shadowing warning (TUR-W0039-style); 390 lines still trips
    the plan's "no module over ~200 lines" smell threshold and the
    `:exports` list under `build.tur` still advertises the Tier 2
    surface (`fm-voice`, `wavetable-*`, `karplus-strong`,
    `poly-synth`, ...) that this plan explicitly defers.
  Practical consequence: the rebuild is no longer a green-field
  scaffold. The work is now (a) reconcile what's already in the tree
  against the Tier-1 surface table below, (b) split `synth.tur` per
  this plan's `osc`/`filter`/`shaper`/`compose` module layout, and
  (c) drop the Tier 2 exports from `build.tur` until each lands behind
  a real consumer.

The "Background" prose below is preserved verbatim for the historical
record but should be read with this audit in mind.

## Status update (2026-06-05, second-pass audit)

A re-audit against the live tree against turmeric `main` at `21d11393`
turns up two regressions the prior status block missed. The plan is NOT
"only paperwork left"; the spice is currently un-runnable end-to-end.

### Regressions found this pass

1. **`make-struct Pair` / `(Pair float float)` type-spec fails inside
   `(defmodule ...)`.** `core.tur:85` (the original `pair-signals`
   body) emits `TUR-E0012` kind mismatch and `make-struct: 'Pair' is
   not a defined struct type`. `shaper.tur:133` (the `mix` body)
   emits `TUR-E0042` mixed-width arithmetic because the
   `(Pair float float)` annotation on `prod-sig`'s return propagates
   to `pair-fst`/`pair-snd` as the int default. Both fixed in-tree by
   switching to the `pair` constructor function and `(as float ...)`
   on the `pair-fst`/`pair-snd` reads.
2. **`^fat name : (fn ...)` annotation lost across the defmodule
   export boundary.** Filed as
   [[defmodule-loses-fat-fn-type-annotation]]. The Phase 1 example
   `01_constant_and_time.tur` does NOT run -- the call site
   `(sample c 0.0)` errors with `expected (fn [] : ?), got ptr<void>`
   because `sample`'s exported `^fat sig : (fn [float] float)`
   collapses to `(fn [] : ?)` at the caller. The same gap blocks
   `compose.tur`'s in-module `(__apply-sf ...)` call site (also
   `expected (fn [] : ?)`) and is hard-required for the Phase 1-5
   test matrix and example matrix.

### Acceptance impact

- The plan's prior status block claimed "Tier 1 surface complete /
  banned patterns purged / Phase 1 example runs / test_core passes."
  Banned-patterns claim still holds (no `__arrow_call1` / `memcpy(&` /
  `__signal_call1` / `::\s+:(int|float)` -- the `(as float ...)` fix
  is not a `::` cast). The Phase 1 example does NOT run today, and
  `test_core` does NOT pass today.
- The previous status block has been preserved below for the historical
  record, with corrections inlined. Treat the per-symbol verdict tables
  as the *intent* the rebuild aimed at; treat the regressions above as
  the *current* delta.

### Updated state of the post-rebuild work items

| Item | State |
|---|---|
| Module split (core/osc/filter/shaper/envelope/compose, each <200 lines) | Done. |
| Tier 1 surface present | Done at the defn level; not exercisable until [[defmodule-loses-fat-fn-type-annotation]] resolves. |
| Banned patterns purged | Done (ban-list grep empty). |
| `build.tur :exports` matches Tier 1 | Done. |
| `tur check` clean across all six modules | Done (2026-06-06). All six modules check-clean after `defmodule-loses-fat-fn-type-annotation` and `vec-typed-fat-closure-readback-fixture-regressed-codegen` resolved. |
| Phase 1 example runs | Done. `01_constant_and_time.tur` runs correctly. |
| `test_core` passes | Done. |
| `test_compose` passes | Done (2026-06-06). Phase 5 `effects-chain` via `__sf-fold` fold, `test_compose.tur` added covering empty/single/two/three-SF chains. |
| Example 02 (`02_oscillators.tur`) runs | Done (2026-06-06). `-lm` autolink added to `osc.tur` and `shaper.tur`. |
| Tests 3-5 + examples 3-5 | Not yet written. |
| Bug reports owed | **Filed:** [[defmodule-loses-fat-fn-type-annotation]]. **Not yet filed:** the `svf-low-pass`-was-removed paper-trail and the `dsp.tur` Pair64 bit-cast obsolete note. |

## Status update (2026-06-05, post-rebuild execution -- preserved for history)

The source-side rebuild has landed in `../turmeric-spices/spices/signal/`.
Verified against the live tree:

- **Module split done.** `src/signal/` now contains `core.tur` (88L),
  `osc.tur` (102L), `filter.tur` (87L), `shaper.tur` (152L),
  `envelope.tur` (95L), `compose.tur` (66L). `dsp.tur` and `synth.tur`
  are gone. Every file is under the ~200-line smell threshold.
- **Tier 1 surface complete.** Spot-checked defns per module match the
  Tier 1 table: `core` has `constant`/`time-signal`/`sample`/
  `map-signal`/`pair-signals`; `osc` has all four waveforms (including
  the previously-missing `triangle`); `shaper` adds `multiply`, `offset`,
  `invert`, `abs-sf`, `scale`, `saturate-tanh`, `clip`; `envelope` owns
  `adsr-gen`; `compose` owns `effects-chain`.
- **Banned patterns purged.** `shaper.tur` `mix`/`add`/`multiply` use
  `pair-fst`/`pair-snd` from `stdlib/pair.tur` (lines 131-150). The old
  `__dsp_pair_*_float` bit-cast helpers are gone. Ban-list grep
  (`__arrow_call1|__signal_call1|memcpy\(&|::\s+:(int|float)`) is empty
  across `src/signal/`.
- **`build.tur` `:exports`** matches the Tier 1 surface exactly --
  `left-signal`/`right-signal` and the entire Tier 2 surface are pruned.

**Remaining work** (the only items still open for this plan):

1. **Tests** -- only `tests/signal/test_core.tur` exists. Still owed:
   `test_osc`, `test_filter`, `test_shaper`, `test_envelope`,
   `test_compose` per Phases 2-5 validation.
2. **Examples** -- only `01_constant_and_time.tur` exists. Still owed:
   `02_oscillators`, `03_filters_and_shapers`, `04_envelopes`,
   `05_simple_voice`.
3. **`tur check` clean run** across all six modules (record verdict).
4. **README pass** -- the spice's `README.md` should be re-read against
   the actual Tier 1 surface that shipped.
5. **Bug reports owed** (per CLAUDE.md "Reporting Bugs" rule, per the
   "Bug reports owed" subsection below):
   - `docs/reported/dsp-pair-bit-cast-helpers-obsolete.md`
   - One-liner noting the `synth.tur` `Phase 0c` placeholders were
     unblocked by PR #281; move
     `docs/reported/let-bound-sf-loses-outer-arg-type-when-inner-captures.md`
     to `docs/archive/`.
   - Note `svf-low-pass` was removed with no consumers.
6. **Phase 6 cross-references** -- update
   `docs/guides/arrows-guide.md` to cite the new spice as a worked
   example; optionally add `docs/guides/signal-spice-guide.md` if the
   spice README outgrows one page.
7. **Plan hygiene** -- once 1-6 land, tick the remaining acceptance
   boxes and move this plan from `docs/upcoming/` to `docs/archive/`.

The "Existing-module gap analysis" and per-symbol verdict tables below
are preserved as the historical record that drove the work above.

## Existing-module gap analysis (2026-06-05)

Per-symbol verdict against the Tier 1 surface table, based on a read of
the current `../turmeric-spices/spices/signal/src/signal/*.tur`.

### `core.tur` -- mostly green; trim Either stubs

| Symbol | Current state | Action |
|---|---|---|
| `constant` | Monomorphic `:float`-only (matches G2 amber workaround) | Keep. Rename plan reference `constant` -> `constant-float`, or document the amber workaround at the export. |
| `time-signal` | `:float -> :float` identity | Keep. |
| `sample` | Typed `^fat sig : (fn [float] float)` -> `:float` | Keep. |
| `map-signal` | Typed `(fn [float] float)` only | Keep for Tier 1. Note: returns `:int` annotation on the outer lambda is a leftover -- should be `(fn [:float] :float)` once the fat-closure return type can be spelled in a defn signature. Low priority. |
| `pair-signals` | Builds `(pair (sig-a t) (sig-b t))` -- typed Pair via stdlib | Keep, but verify it actually returns `(Pair :float :float)` through fat dispatch (G4 fixture passes; should work). The current `:int` return annotation also wants tightening. |
| `left-signal` / `right-signal` | Identity stubs ("Either not yet implemented") | **Drop.** Not on the Tier 1 list. Their `build.tur` `:exports` entry also drops. |

### `dsp.tur` -- partial Tier 1; one banned pattern, several gaps

| Tier 1 symbol | Current state | Action |
|---|---|---|
| `sine` | Implemented inline (no LUT); typed `:float -> :float` end | Keep. Move to `osc.tur` per layout. |
| `square` | Implemented (duty cycle); typed | Keep. Move to `osc.tur`. |
| `sawtooth` | Implemented; typed | Keep. Move to `osc.tur`. |
| `triangle` | **Missing.** | New. Add to `osc.tur`. |
| `low-pass` | Typed `:ptr<:float>` state, inline-C with `double *` (G5 green path) | Keep. Move to `filter.tur`. |
| `high-pass` | Typed `:ptr<:float>` state, inline-C with `double *` | Keep. Move to `filter.tur`. |
| `gain` | Closure over scalar; typed | Keep. Move to `shaper.tur`. |
| `mix` | **Banned pattern**: `__dsp_pair_first_float` / `__dsp_pair_second_float` use `memcpy(&v, &bits, 8)` on a manually laid-out `Pair64 { int64_t first; int64_t second; }`. The G4 verdict makes this unnecessary -- use `pair-fst` / `pair-snd` from `stdlib/pair.tur`. | **Re-type.** Replace the inline-C accessors with `stdlib/pair.tur`. Move to `shaper.tur`. |
| `add` | Same banned `__dsp_pair_*_float` shape as `mix`. | **Re-type** alongside `mix`. Move to `shaper.tur`. |
| `multiply` | **Missing.** | New. Implement alongside `mix`/`add` over typed `Pair`. |
| `offset` | **Missing.** | New. `shaper.tur`. |
| `invert` | **Missing.** | New. Trivial: `(* -1.0 (sig t))`. `shaper.tur`. |
| `abs-sf` | **Missing.** | New. `shaper.tur`. |
| `scale` (pure) | **Missing.** | New. `shaper.tur`. |
| `saturate-tanh` | **Missing.** | New. `shaper.tur`. |
| `hard-clip` | **Lives in `synth.tur`** as a working SF using `__sample-clip` | Keep, **move to `shaper.tur`**. |
| `clip lo hi` (asymmetric) | **Missing.** | New. `shaper.tur`. |

### `envelope.tur` -- green

| Symbol | Current state | Action |
|---|---|---|
| `ADSRParams` | `defstruct` with 4 `:float` fields | Keep. |
| `__adsr_attack` / `_decay` / `_sustain` / `_release` | Inline-C `(intptr_t)` casts on the struct pointer | Acceptable per plan ("typed struct + accessors") but the `(intptr_t)` and `ADSRParams_` redeclaration is a code smell. Worth a follow-up to use `(. p attack)` syntax once that's reliable for fat-closure-captured structs. **Keep for now.** |
| `__adsr_fixed_sample` | Pure inline-C ADSR math | Keep. |
| `adsr-fixed` | Typed SF | Keep. |
| `adsr-gen` | **Lives in `synth.tur`** as a 1-line wrapper `(adsr-fixed params 1.0)` | **Move to `envelope.tur`** per layout. |

### `synth.tur` -- mostly placeholders; the blocker has been fixed

`synth.tur` is 390 lines and the bulk is placeholders returning
`(constant 0.0)` (`voice`, `voice-sf`, `poly-synth`, `fm-voice`,
`wavetable-voice`, `granular`, `additive-voice`, `karplus-strong`,
`ks-voice`, `lead-synth`, `bass-synth`, `pad-synth`, `pluck-synth`,
`effects-chain`, `example-effects-chain`, `step-sequencer`). Several
"wavetable" generators (`sine-wavetable`, `square-wavetable`,
`sawtooth-wavetable`) return a vec of zeros instead of a waveform. All
of these are exactly the "half-stub primitives" the plan bans and the
"reporting bugs" CLAUDE.md rule forbids.

Critically, every placeholder cites the same blocker:

> Phase 0c note: the SF-composition path uses captured closures whose
> types the elaborator cannot infer through nested let-bindings.
> Tracked in
> `docs/reported/let-bound-sf-loses-outer-arg-type-when-inner-captures.md`.

**That blocker has been resolved.** PR #281 (commit `50903134`,
2026-06-05) is titled "Fix let-bound SF type-check: route thin
closure-returning calls correctly" and references the very report each
placeholder cites. The report is still in `docs/reported/` (not yet
moved to archive) but the fix has landed. The placeholder rationale is
now stale.

Verdict per `synth.tur` symbol:

| Symbol | Current state | Tier | Action |
|---|---|---|---|
| `__sample-add` / `__sample-mul` / `__sample-clip` | Real, typed | -- | Keep as `shaper.tur` internals (or inline into `clip`/`hard-clip`). |
| `hard-clip` | Real, typed | 1 | **Move to `shaper.tur`**. |
| `adsr-gen` | Real, 1-line wrapper | 1 | **Move to `envelope.tur`**. |
| `svf-low-pass` | Wraps `low-pass`, throws away `q` -- a lie about resonance | 2 | **Drop.** Resonant filters are Tier 2. |
| `voice` / `voice-sf` / `poly-synth` | Placeholder stubs | 2 | **Drop.** Tier 2. |
| `fm-voice` | Wraps `voice` placeholder | 2 | **Drop.** Tier 2. |
| `wavetable-osc` / `wavetable-voice` | Stubs (osc just calls `sine`) | 2 | **Drop.** Tier 2. |
| `sine-wavetable` / `square-wavetable` / `sawtooth-wavetable` | Return zero-filled vecs | 2 | **Drop.** Tier 2. |
| `granular` / `additive-voice` / `karplus-strong` / `ks-voice` | Stubs | 2 | **Drop.** Tier 2. |
| `lead-synth` / `bass-synth` / `pad-synth` / `pluck-synth` | Wrap `voice` placeholder | 2 | **Drop.** Tier 2. |
| `effects-chain` / `example-effects-chain` | Identity stubs | 1 (just `effects-chain`) | **Rewrite** using `stdlib/arrow.tur`'s `compose-float` over `Vec<SF Sample Sample>`. Move to `compose.tur`. |
| `step-sequencer` | Stub | 2 | **Drop.** Tier 2. |

### `build.tur` `:exports` -- needs pruning to Tier 1

Currently exports the full Tier 2 surface (`fm-voice`, `wavetable-*`,
`granular`, `additive-voice`, `karplus-strong`, `ks-voice`,
`lead-synth`/`bass-synth`/`pad-synth`/`pluck-synth`, `poly-synth`,
`step-sequencer`, `example-effects-chain`) plus `left-signal` /
`right-signal`. The acceptance criterion "`build.tur` `:exports`
matches every public symbol in `src/signal/`, and no extras" therefore
requires both a Tier 2 prune **and** a re-key from the current
`signal/core` / `signal/dsp` / `signal/envelope` / `signal/synth` map
to the new `signal/{core,osc,filter,shaper,envelope,compose}` modules.

### Net work to reach Tier 1

1. **`core.tur`**: drop `left-signal` / `right-signal`; tighten the
   `:int` outer-lambda annotations on `map-signal` / `pair-signals` if
   the elaborator now accepts the full type. No new symbols.
2. **`osc.tur`** (new file): move `sine`, `square`, `sawtooth` from
   `dsp.tur`; **add** `triangle`.
3. **`filter.tur`** (new file): move `low-pass`, `high-pass` from
   `dsp.tur`.
4. **`shaper.tur`** (new file): move `gain` from `dsp.tur`, `hard-clip`
   from `synth.tur`; **re-type** `mix` and `add` to use
   `stdlib/pair.tur`'s `pair-fst` / `pair-snd` instead of the banned
   `__dsp_pair_*_float` bit-cast helpers; **add** `multiply`, `offset`,
   `invert`, `abs-sf`, `scale` (pure), `saturate-tanh`, `clip lo hi`.
5. **`envelope.tur`**: move `adsr-gen` here from `synth.tur`. No new
   symbols.
6. **`compose.tur`** (new file): rewrite `effects-chain` using
   `compose-float` over `Vec<SF Sample Sample>`. Drop
   `example-effects-chain` (the example belongs under `examples/`).
7. **Delete `dsp.tur`** once `osc.tur` / `filter.tur` / `shaper.tur` own
   its contents.
8. **Delete the Tier 2 surface from `synth.tur`** (all the placeholders
   and Tier 2 wrappers). What remains -- `hard-clip`, `adsr-gen`, the
   sample helpers -- moves out per (4) and (5), so `synth.tur` itself
   is deleted. This brings the spice well under the ~200 lines/module
   smell threshold.
9. **`build.tur`**: re-key `:exports` to the new module names and prune
   to exactly the Tier 1 surface.
10. **Examples + tests**: replace the existing `examples/01_basics.tur`
    / `02_signals.tur` / `03_dsp.tur` tutorial with the
    per-phase `01_constant_and_time.tur` ... `05_simple_voice.tur` set
    in the plan; add the `tests/signal/test_*.tur` matrix.

### Bug reports owed (per CLAUDE.md "Reporting Bugs" rule)

Even after the rebuild, these surfaced while reading and should not be
swept under the rug:

- **`dsp.tur` pair-handling pattern.** `__dsp_pair_first_float` /
  `__dsp_pair_second_float` exist because the original author didn't
  know typed `Pair` would survive fat dispatch. The G4 fixture proves
  it now does -- but the existing tree still ships the banned pattern.
  File a `docs/reported/dsp-pair-bit-cast-helpers-obsolete.md` noting
  the helpers are obsolete and the rebuild deletes them.
- **Stale `Phase 0c note` placeholders in `synth.tur`.** The block of
  `(constant 0.0)` returns is justified by a report (`let-bound-sf-...`)
  that PR #281 resolved. File a one-line note that the placeholders are
  unblocked and tracked under this rebuild plan, and ensure the
  resolved report is moved to `docs/archive/` in the same change set
  that does the rebuild work.
- **`svf-low-pass` semantic lie.** It accepts a `q` parameter and
  silently throws it away. Tier 2 will eventually ship a real SVF, but
  shipping a name that promises resonance and delivers a 1-pole filter
  is the half-stub anti-pattern. Drop it; if a `docs/reported/` entry
  is wanted for paper-trail, file one noting it was removed without
  consumers.

## Background

The previous `tur-signal` spice was removed wholesale (commit accompanying
this plan; the tree at `../turmeric-spices/spices/signal/` no longer
exists). The reasons are documented in
`docs/archive/history/signal-spice-broken-build.md` (resolved) and revisited briefly here:

- The build did not pass against the current `tur` / stdlib. Symbols the
  spice depended on (`__arrow_call1`, `vec-length`, `vec-create`,
  `vec-of` for heterogeneous closures, `ADSRParams` as a callable
  constructor) had either been removed, renamed, or never typed the way
  the spice assumed.
- The remaining live code was built on a sample ABI that pre-dated the
  typeclass + fat-closure work in the main repo: every sample was an
  `int64_t` bit-pattern of a `double`, every signal-call was a raw
  `int64_t(*)(int64_t)` cast, and every pair was a manually-laid-out
  `{int64_t e1; int64_t e2;}` walked through `(intptr_t)` casts.
- An attempt to restore the build with the minimal possible diff
  ([[signal-primitives-expansion-plan]] Phase 0a, executed and reverted
  in the session that produced this doc) demonstrated that the old shape
  cannot be patched forward without reintroducing exactly the patterns
  the recent infrastructure work eliminated.

This plan rebuilds the spice in the shape that the *current* language
expresses cleanly, with no carry-over of the old ABI.

## Hard prerequisites

This plan does **not start** until
[[language-readiness-for-typed-signal-plan]] has a verdict block filled
in for every gap G1-G8. Specifically:

- **G1 (`:float`-fat-closures)** must be green. The signal ABI depends
  on it.
- **G7 (typed `>>>` composition)** must be green. The library's central
  combinator depends on it.
- **G3 (`vec-of` over SFs)** must be at least amber with a documented
  workaround. If red, the effects-chain shape changes (see "Open
  questions" below).
- **G4 (typed Pair through closures)** must be green for `pair-signals`
  to be expressible cleanly. If amber, document the workaround at the
  call site.
- **G2 (polymorphic `constant`)**, **G5 (typed state cells)**, **G6
  (Vec[Closure] reads)** can be amber; the rebuild documents the
  workarounds per affected primitive.

If any hard prerequisite is red and unfixable in the readiness plan's
own scope, this rebuild does not start. Instead the readiness plan grows
a fix-it sub-phase.

## Scope of the rebuild

A minimal, *typed* signal library that re-establishes the surface area
the old spice claimed, minus the stubs that lied about their semantics.
Specifically:

### Module layout (target)

```
../turmeric-spices/spices/signal/
  build.tur                # :exports list, :spices deps
  README.md                # public-facing user docs
  src/signal/
    core.tur               # Signal, SF, constant, time-signal,
                           # pair-signals, map-signal, sample
    osc.tur                # sine, square, sawtooth, triangle
    filter.tur             # low-pass, high-pass (first-order)
    shaper.tur             # gain, mix, add, scale, saturate-tanh,
                           # hard-clip, abs-sf, invert, offset
    envelope.tur           # ADSRParams, adsr-fixed, adsr-gen
    compose.tur            # effects-chain over Vec<SF Sample Sample>
                           # (shape depends on G3 outcome)
  examples/
    01_constant_and_time.tur
    02_oscillators.tur
    03_filters_and_shapers.tur
    04_envelopes.tur
    05_simple_voice.tur
  tests/signal/
    test_core.tur
    test_osc.tur
    test_filter.tur
    test_shaper.tur
    test_envelope.tur
    test_compose.tur
```

The old single-file `synth.tur` is split into `osc`, `filter`, `shaper`,
`envelope`, `compose` because the old file's monolithic shape was part
of why drift went undetected -- importers couldn't tell which subset of
its 440 lines they were depending on.

### Types

```turmeric
;; In a stdlib type-alias form if G2 lands; otherwise spell out
;; (-> :float A) at every signature.
type Time   = :float
type Sample = :float
type Signal = (fn (:float) A)             ; A varies per call site
type SF     = (fn ((Signal A)) (Signal B))
```

Concrete:

- `Signal Sample` = `(fn (:float) :float)`.
- `Signal Bool` = `(fn (:float) :bool)` (for gates).
- `SF Sample Sample` = a function from a sample-signal to a sample-signal.

The library *uses* these everywhere. No `:int` boundary casts. No
`memcpy(&x, &sig_val, 8)` bit reinterpretation. If a primitive needs a
boundary cast in inline-C, that is a finding -- file it back into
[[language-readiness-for-typed-signal-plan]] as a new gap.

### Surface area (Tier 1 -- the rebuild ships when this works)

| Symbol | Module | What it produces |
|---|---|---|
| `constant val` | core | `Signal A` (poly if G2 green; else per-type) |
| `time-signal` | core | `Signal Time` |
| `sample sig t` | core | invoke a `Signal A` at time `t` |
| `map-signal f sig` | core | `Signal A -> Signal B` |
| `pair-signals sa sb` | core | `Signal (Pair A B)` |
| `sine freq phase` | osc | `SF () Sample` |
| `square freq duty` | osc | `SF () Sample` |
| `sawtooth freq` | osc | `SF () Sample` |
| `triangle freq` | osc | `SF () Sample` |
| `low-pass alpha` | filter | `SF Sample Sample` |
| `high-pass alpha` | filter | `SF Sample Sample` |
| `gain g` | shaper | `SF Sample Sample` |
| `offset c` | shaper | `SF Sample Sample` |
| `invert` | shaper | `SF Sample Sample` |
| `abs-sf` | shaper | `SF Sample Sample` |
| `mix alpha` | shaper | `SF (Pair Sample Sample) Sample` |
| `add` | shaper | `SF (Pair Sample Sample) Sample` |
| `multiply` | shaper | `SF (Pair Sample Sample) Sample` |
| `scale in-lo in-hi out-lo out-hi` | shaper | pure `:float -> :float`; `scale-sf` lifts |
| `saturate-tanh drive` | shaper | `SF Sample Sample` |
| `hard-clip limit` | shaper | `SF Sample Sample` |
| `clip lo hi` | shaper | `SF Sample Sample` (asymmetric) |
| `ADSRParams attack decay sustain release` | envelope | typed struct |
| `adsr-fixed params gate-duration` | envelope | `SF () Sample` |
| `adsr-gen params` | envelope | `(gate-signal -> SF () Sample)` |
| `effects-chain effects input` | compose | composition of `Vec<SF Sample Sample>` |

Tier 1 is what an honest version of the previous spice would have
shipped. Anything beyond -- resonant filters, bitcrush, comb, wavetable,
FM voice, FFT -- is **explicitly out of scope** until Tier 1 lands clean
and there is a real consumer.

### Surface area (Tier 2 -- follow-up, not part of this plan)

Tracked but deferred:

- Ladder filters (3p, 4p) with resonance.
- Bitcrusher, comb-{ff,fb}, wavetable osc + LUT, FM synth.
- FFT primitives.
- Granular, additive, step sequencer.
- A real `voice` / `voice-sf` / `poly-synth` composition.

Each of these gets its own plan once Tier 1 has at least one external
consumer. Adding them speculatively is what produced the previous
spice's stub graveyard.

### Examples and tests

- Each `examples/0N_*.tur` runs end-to-end and prints sample values that
  match hand-computed references in comments. No stub examples that
  silently return zero.
- Each `tests/signal/test_*.tur` covers its module's surface with at
  least:
  - One round-trip-correctness test (e.g. `(sample (constant 0.5) 0.0)
    => 0.5`).
  - For stateful SFs (`low-pass`, `high-pass`, ADSR): impulse-response
    over the first ~8 samples vs. hand-computed values.
  - For pure SFs: input/output assertions at a few specific times.
- Test harness uses the same idiom as the rest of `turmeric-spices` --
  whatever `tests/run.sh` or the spice's own `tur run test` resolves to
  in the current build. Confirm the runner exists before committing.

## What is explicitly NOT carried over from the old spice

A non-exhaustive ban list, written down so reviewers can grep:

- `__arrow_call1` (raw `int64_t(*)(int64_t)` cast). The new library calls
  closures directly via `^fat` dispatch, or through `>>>` / `arrow-first`
  / etc. from `stdlib/arrow.tur` (post-scale-back).
- `__signal_call1` (the same pattern under a different name).
- Manual `{int64_t e1; int64_t e2;}` C structs as a pair representation.
  Use `stdlib/pair.tur`'s typed `Pair[A B]`.
- `(:: x :int)` / `(:: x :float)` boundary casts on sample values.
  Samples are `:float`; no bit-pattern dance.
- `union {double d; int64_t i;} u;` helpers (the `__f_0_5` family in the
  old tests). Tests use real `:float` literals.
- `vec-create n 0` and other not-actually-in-stdlib helpers. The new
  library uses `vec-new` + `vec-push!` or, where it actually needs a
  preallocated vec, defines exactly one local helper with a clear
  docstring.
- "Half-stub" primitives. If a function's body doesn't do what its
  docstring claims, it ships disabled (or not at all), not as a lie.
- A monolithic `synth.tur`. Split per category; `:exports` per file.

## Phasing

### Phase 1 -- Core (depends: G1, G2, G4)

`src/signal/core.tur` with `constant`, `time-signal`, `sample`,
`map-signal`, `pair-signals`. Tests in `tests/signal/test_core.tur`.

Validation:
- `tur check` clean.
- `bash tests/run.sh` or the equivalent passes the new core tests.
- `examples/01_constant_and_time.tur` runs and matches hand-computed
  output.
- No `:int` boundary casts in the generated C for any sample expression.

### Phase 2 -- Oscillators (depends: Phase 1)

`src/signal/osc.tur` with `sine`, `square`, `sawtooth`, `triangle`.
Tests covering each at 2-3 time points + zero-crossing assertions.

Phase 2 ships when:
- `(sine 1.0 0.0)` at `t=0.0` is `0.0` (within tolerance).
- `(sine 1.0 0.0)` at `t=0.25` is `1.0`.
- `(square 1.0 0.5)` flips at the expected boundary.
- `sawtooth` ramps in `[-1, 1)`.
- `triangle` peaks at the midpoint.

### Phase 3 -- Filters + Shapers (depends: Phase 2, G5)

`src/signal/filter.tur`, `src/signal/shaper.tur`. Impulse-response tests
on the filters; input/output pair tests on the shapers.

If G5 was amber (typed state cells didn't quite land), the filters
ship with `:ptr<void>` state plus a one-line comment per filter
explaining the workaround. The comment links to the gap's verdict block
in [[language-readiness-for-typed-signal-plan]].

### Phase 4 -- Envelopes (depends: Phase 1)

`src/signal/envelope.tur`. The old `adsr-fixed` worked; bring it back
typed. `adsr-gen` is a thin wrapper. No gate-driven re-trigger logic in
this rebuild -- that lands when there is a real voice consumer.

### Phase 5 -- Composition (depends: Phases 2-4, G3, G6, G7)

`src/signal/compose.tur` with `effects-chain`. Shape depends on G3:

- **G3 green**: `effects-chain` takes `Vec<SF Sample Sample>` and
  uses `>>>` from `stdlib/arrow.tur`.
- **G3 amber**: `effects-chain` takes a typed singly-linked
  `List<SF Sample Sample>` and uses `>>>`. The example doc explains
  why and links to the gap verdict.
- **G3 red**: do not ship `effects-chain`. The library uses inline
  composition only; document the limitation in the README.

**Status (2026-06-06, updated):** `effects-chain` is implemented and passing
`test_compose` via a `__sf-fold` recursive pattern that applies each SF to the
accumulated signal directly. The `fat-shim-void-ptr-calls-bare-not-fat` gap has
been resolved (the compiler now emits `__tur_poly_to_fat` passthrough shims for
`^fat` let-bindings of runtime `ptr<void>` fat closures, replacing the broken
one-arg shim). However, `(load "stdlib/arrow.tur")` is not visible inside a
`defmodule` scope, so `>>>` cannot be called from within `compose.tur`. The
`__sf-fold` pattern achieves equivalent composition directly: it applies each
SF to the accumulated signal in left-to-right order using `^fat` parameters
(which use correct fat dispatch through `sf[0]` at each step). Phase 5 done.

### Status update (2026-06-07, sixth-pass -- blocker 1 partially resolved, new sub-bug found)

**Blocker 1 -- captureless SFs segfault through the `ptr<void>` `^fat` boundary.**
Filed: [[captureless-closure-not-boxed-at-fat-ptr-void-boundary]].
**PARTIALLY RESOLVED.** The ascription-stripping fix (`elab_call.c`) resolves
the *direct-ascription* case: `(:: (invert) :ptr<void>)` as a `^fat` argument
now correctly shims via `EX_FN_TO_FAT`. Fixture `fat-captureless-closure-ptr-void`
passes (`-0.5 -0.5`). `>>>` applied directly to `(invert)` also works.

**New sub-bug found (RESOLVED).** When a captureless closure is stored in a
`Vec` (e.g. `(vec-of (gain 2.0) (invert))`), its bare fn-pointer was cast to
`int64_t` at `vec-push!` time. `vec-get` retrieved it as opaque `TY_INT` --
fn-pointer provenance was gone. A subsequent `^fat` cast then fat-dispatched the
code address as a fat-box slot 0 -> segfault. The ascription-stripping fix never
fired because no `EX_ASCRIBE` wrapper survives `vec-get`. Filed and fixed:
[[captureless-closure-lost-through-untyped-vec]]. The elaborator now boxes a
captureless closure into a uniform `{ shim, fn }` fat box (`EX_FN_TO_FAT`) at the
point it escapes into a polymorphic (`TY_TYVAR`) carrier such as the `val :A`
parameter of `vec-push!`, so the stored value is a valid fat box regardless of
capture. Fixture: `tests/fixtures/vec-captureless-fat-closure-readback/`.

**Blocker 2 -- `(load "stdlib/arrow.tur")` unreachable from imported module.**
Filed: [[load-not-expanded-in-imported-or-project-modules]].
**RESOLVED (import path, 2026-06-06, #305).** Fixture: `tests/fixtures/load-in-imported-module/`.

**Phase 5 complete.** `compose.tur` uses `(load "stdlib/arrow.tur")` + `>>>`.
`test_compose` covers empty, single, two, and three-SF capturing chains. All
pass. Captureless SF coverage (`invert` via `effects-chain`) is now unblocked --
[[captureless-closure-lost-through-untyped-vec]] is fixed (captureless closures
are boxed into a uniform fat box when they escape into a `Vec`).

### Phase 6 -- README + arrows-guide cross-references (depends: Phases 1-5)

- Write `../turmeric-spices/spices/signal/README.md` to match what
  actually ships. Every example is copy-pasteable.
- Update `docs/guides/arrows-guide.md` (in this repo) to reference the
  new signal spice as a worked example consumer -- *after*
  [[stdlib-arrow-scaleback-plan]] has landed.
- File a brief `docs/guides/signal-spice-guide.md` if the README ends up
  needing more than a page; otherwise the README is enough.

## Acceptance for the rebuild

- [x] Every hard prerequisite in
      [[language-readiness-for-typed-signal-plan]] is green or amber
      with a documented workaround in this plan. *(Prereq audit
      2026-06-05 above.)*
- [x] Every file under `src/signal/` checks clean under `tur check`.
      *(Re-audit 2026-06-06: all 6 modules clean. [[defmodule-loses-fat-fn-type-annotation]]
      is resolved and archived.)*
- [ ] Every example under `examples/` runs and prints values matching
      its in-file comments. *(Re-audit 2026-06-06: `01_constant_and_time.tur`
      and `02_oscillators.tur` run; `02` requires `-lm` linkage fix in
      `osc.tur` (add `__tur_autolink__: -lm`). Examples 03-05 still owed.)*
- [ ] Every test under `tests/signal/` passes; zero `FAIL` lines.
      *(Re-audit 2026-06-06: `test_core` passes. Tests 2-5 still owed.)*
- [x] `grep -rE "__arrow_call1|__signal_call1|memcpy\(&|::\s+:(int|float)" src/signal/`
      is empty. *(Verified 2026-06-05.)*
- [x] No file in `src/signal/` exceeds ~200 lines. *(Max is shaper.tur
      at 152L.)*
- [ ] README documents the surface area and matches reality. *(README
      exists; needs a pass against the actually-shipped Tier 1 surface.)*
- [x] `build.tur` `:exports` matches every public symbol in `src/signal/`,
      and no extras. *(Verified 2026-06-05.)*

## Validation gates (per phase, runnable)

```sh
# from turmeric-spices repo root
cd spices/signal
tur check src/signal/*.tur                       # all clean
tur run tests/signal/test_*.tur                  # zero failures
tur run examples/01_constant_and_time.tur        # phase 1 smoke
# ...etc. per phase
```

Plus, in the main turmeric repo:

```sh
bash tests/run.sh                                # main suite still green
```

## Non-goals

- Polyphony, voice allocation, real-time audio backend, WAV I/O, MIDI.
  All explicitly out of scope.
- Performance optimisation. Correctness first; profile only if a real
  consumer demands it.
- A second spice for "advanced DSP" (resonant filters, wavetable, FM,
  FFT). Tier 2. Separate plans, separate consumer demand.
- Touching `stdlib/arrow.tur` in this plan -- that is the job of
  [[stdlib-arrow-scaleback-plan]]. This rebuild *consumes* whatever
  scale-back ships.
- Reintroducing the old `examples/01_basics.tur` / `02_signals.tur` /
  `03_dsp.tur` tutorial sequence verbatim. The new examples are shorter,
  focused per-module, and actually compile.

## Open questions

- **Where does `Signal` / `SF` live?** Options:
  - In the spice's `src/signal/core.tur` as locally-defined type
    aliases.
  - As a stdlib type module (`stdlib/signal-types.tur`?) if other
    spices end up wanting it.
  - Spelled out at every signature.
  Default: define locally in the spice for now; promote to stdlib only
  if a second consumer materialises.
- **Sample rate plumbing.** Hard-code `48000.0` like the old plan
  proposed, or use a dynvar (`*signal-sample-rate*`) from day one?
  Default: hard-code, with the constant exported so callers can read it.
  Promote to dynvar in a follow-up if a consumer wants per-instance SR.
- **`Bool` for gates.** `Signal Bool` requires fat-dispatch over a
  `:bool`-returning closure. If G1 succeeds for `:float` but
  `:bool`-returning closures hit a separate codegen issue, gates
  degrade to `Signal :int` with the convention `0 = off, 1 = on` --
  same as the old library, but typed.
- **Test runner.** Confirm `bash tests/run.sh` (or whatever the spice
  repo's harness is) runs spice-level tests. The previous spice carried
  tests but had no clear runner integration in its build.tur; do not
  inherit that confusion.

## Cross-references

- Supersedes `docs/archive/history/signal-primitives-expansion-plan.md`
  (the archived version of the old plan).
- Direct prerequisite: [[language-readiness-for-typed-signal-plan]].
- Direct prerequisite: [[stdlib-arrow-scaleback-plan]].
- Findings that triggered the rebuild:
  `docs/archive/history/signal-spice-broken-build.md` (resolved).
