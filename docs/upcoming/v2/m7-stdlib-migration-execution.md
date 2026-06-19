---
title: M7 stdlib HKT migration -- concrete execution sequencing
category: Planning -- M7 follow-through
description: How to take the flag-gated by-value HKT machinery (probe-hardened for 7 of 9 method shapes) into the stdlib classes/instances and flip TUR_M7_HKT on by default. Resolves the "how" the parent plan's Phase 3/4.2 leaves open.
---

# M7 stdlib HKT migration -- concrete execution sequencing

**Status:** design, ready to execute. Predecessor work: the layer-4 by-value
emit is probe-hardened for 7 of the 9 HKT classes' primary method shapes
(`fmap`, `bind`, `ap`, `pure`, `<|>`, `extract`, `foldr` -- probes under
`docs/upcoming/v2/m7-hkt-probe-*.tur`, all exit their target under
`TUR_M7_HKT=1`). Bifunctor `bimap` and Traversable `traverse` are reported as
blocked (`docs/reported/m7-hkt-{bimap-twoparam-struct-tyvar-leak,
traverse-method-level-hkt-tyvar}.md`).

## The two hard constraints

1. **The stdlib HKT class signatures are shared source.** Changing
   `Functor`'s decl from `(fmap [container [fn :fn]] : int)` to
   `(fmap [container : (f a) fn : (fn [a] b)] : (f b))` changes how the file
   PARSES. The M7 parse additions (method-level tyvar collection, HKT param
   kind threading, bare-element return) are flag-gated, so **flag-off the new
   signature does not parse** (`a`/`b` are unknown). Therefore a stdlib sig
   change cannot be committed while the suite runs flag-off -- it is only valid
   once the flag is on.

2. **Instance BODIES cannot be flag-straddled.** A by-value body
   (`(if (some? c) (some (f (.value c))) (none))`) requires `c : (Option a)`;
   flag-off the param collapses to the int carrier, so `(.value c)` does not
   typecheck. So a body is EITHER inline-C carrier (works both flag states, stays
   carrier) OR pure-Turmeric by-value (needs flag-on). It cannot be both.

**Conclusion:** the migration is a single coordinated flip of `TUR_M7_HKT` to
default-ON, landing the sig changes + body rewrites together, then (once stable)
removing the flag and the flag-off path. It is NOT incrementally flag-off-safe.

## The de-risking lever (verified 2026-06-19)

An HKT class with the **new applied signature** but an **inline-C carrier body**
stays on the carrier ABI even flag-on (the layer-4 gate excludes inline-C and
carrier-delegating bodies; the by-value result type is then not committed, so
consumers see the carrier too -- self-consistent). Verified: a `(fm [container :
(f a) fn : (fn [a] b)] : (f b))` class with an inline-C Option body runs flag-on
with **zero** `__spec` clones and the correct result.

**So the flip does not require EVERY instance to be by-value.** Each instance is
independently either:
- **rewritten** to a pure-Turmeric by-value body (the 7 hardened shapes), or
- **left inline-C carrier** (bimap/traverse and any not-yet-hardened instance),
  staying on the uniform carrier ABI exactly as today.

This removes the "atomic across all 9 classes / 35 instances" framing: the flip
must upgrade all SIGNATURES at once (so the file parses), but BODIES migrate
opportunistically -- carrier bodies are a valid resting state.

## Execution order

### Step 0 -- prerequisite: keep the flag's default OFF until the very end
Do all of the below on a branch with `TUR_M7_HKT` still defaulting off, running
the suite **flag-on** (`TUR_M7_HKT=1 bash tests/run.sh`, 600000ms timeout) as
the gating signal. Only Step 5 flips the default.

### Step 1 -- upgrade all 9 HKT class signatures to the applied form
`stdlib/typeclass*.tur`, `stdlib/comonad.tur`. For each method, replace the
`: int` carrier return and the untyped/`:fn` params with the applied form:
- Functor `fmap [container : (f a) fn : (fn [a] b)] : (f b)`
- Applicative `pure [x : a] : (f a)`, `ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)`
- Monad `bind [ma : (m a) k : (fn [a] (m b))] : (m b)`
- Alternative `empty : (f a)`, `<|>`/`or-else [x : (f a) y : (f a)] : (f a)`
- Foldable `foldr [g : (fn [a b] b) z : b t : (f a)] : b` (bare-element result)
- Comonad `extract [w : (f a)] : a`, `extend`/`duplicate` (HKT result)
- Bifunctor `bimap [g : (fn [a] c) h : (fn [b] d) x : (p a b)] : (p c d)`
- MonadError, Traversable similarly.
Match the probe signatures exactly (they are the validated templates). After
this step the files parse only flag-on.

### Step 2 -- rewrite the by-value-capable instance bodies
For each instance of Functor/Monad/Applicative/Alternative/Comonad/Foldable on
Option/Result/Either/Parser/Goal/Backtrack/Schema/Cons/rc, replace the inline-C
carrier body with the pure-Turmeric by-value body (templates = the probes'
bodies). One instance per commit where practical; suite **flag-on** stays green
after each.

### Step 3 -- leave the blocked/hard instances inline-C carrier
Bifunctor `[Result]` (two-param leak) and Traversable instances (method-level
HKT tyvar + nested result) keep their inline-C carrier bodies. With the upgraded
sig they parse flag-on and stay carrier (the de-risking lever). Annotate each
with a `;;` NOTE pointing at its report. They become by-value when their
reported blockers are fixed.

### Step 4 -- regenerate fixture snapshots
The sig + body changes are a large codegen change. Regenerate all
`tests/fixtures/*/expected.c` per the CLAUDE.md "Fixture Snapshots" recipe, in
the same change. Confirm `bash tests/run.sh` flag-on is clean.

### Step 5 -- flip the default and retire the flag-off path
- Flip `g_m7_hkt_enabled` default to ON (`src/runtime/globals.c` / `main.c`).
- Run the suite with NO env override (now exercising the on path by default);
  zero FAIL.
- Remove the now-dead flag-off branches in `elab_typeclasses.c` /
  `emit_*.c` (the `if (g_m7_hkt_enabled)` guards), and the `TUR_M7_HKT` plumbing,
  once green. This is also the gate that unblocks parent-plan Phase 5 (carrier
  bridge deletion), since HKT dispatch no longer round-trips the carrier except
  at the annotated carrier-essential (Step 3) sites.

## Validation gates (every step)
- `TUR_M7_HKT=1 bash tests/run.sh` -> 0 FAIL (Steps 1-4), then plain
  `bash tests/run.sh` -> 0 FAIL (Step 5).
- The 7 shape probes still exit their targets.
- Spice suites (`../turmeric-spices/`) build, per parent plan 3.3.
- `TUR_M3_AUDIT=1` per-fixture sweep: HKT method boundaries show no carrier
  crossings except at the Step-3 carrier-essential instances.

## Risks
- **Snapshot churn is large** (sig change touches every HKT-using fixture).
  Coordinate timing per the CLAUDE.md "Fixture churn" rule; land the regen in the
  same change, not a follow-up.
- **Parser-instance / recursive instances** (Parser/Goal/Backtrack) were the
  ones that broke the earlier unconditional-gate experiment (parent plan 3.1).
  Migrate them last and watch the flag-on suite closely; if a recursive
  by-value body regresses, leave it inline-C carrier (Step 3) and report.
