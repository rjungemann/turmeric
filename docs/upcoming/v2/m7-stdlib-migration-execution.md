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

> **UPDATE (2026-06-19): default flipped FIRST.** Because the flag-on suite is
> already fully green (1684/0), the default was flipped to ON *ahead of* any
> stdlib sig change (`g_m7_hkt_enabled = true`; `TUR_M7_HKT=0` opts back out).
> Both `bash tests/run.sh` (now flag-on) and `TUR_M7_HKT=0 bash tests/run.sh`
> (legacy carrier) pass 1684/0. This converts the migration from a single giant
> atomic change into an INCREMENTAL one: with the default suite now flag-on,
> stdlib sig upgrades + body rewrites parse and can be committed in small steps,
> each gated by the (default) flag-on suite. The "flip last" framing below is
> superseded by "flip first, migrate incrementally, retire the flag last."

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

## Per-class findings (measured 2026-06-19 while migrating Foldable)

Empirical constraints discovered doing the first real stdlib migration. These
make each Functor-family class a careful per-instance job, not a mechanical
sig swap:

1. **The fold/map FN parameter representation is the central hazard.** The
   legacy sigs leave the fn UNtyped (defaults to the int64 carrier) or `:fn`
   (the `tur_poly_fn_t` poly carrier). Existing inline-C bodies exploit one of
   these two shapes -- either `((int64_t(*)(...))(intptr_t)fn)(...)` (raw fn
   pointer; assumes the untyped-int default) or `fn.fn(fn.env, ...)` (poly
   carrier; assumes `:fn`). Changing the param to the typed `(fn [a] b)` makes
   `fn` lower to a scalar callable pointer, which:
   - KEEPS the raw-fn-pointer inline-C bodies working (verified: Foldable [rc],
     and the `fold2arg`/`carrier-keep` probes) -- so those instances ride the
     de-risking lever and stay carrier; BUT
   - BREAKS the `fn.fn(fn.env, ...)` poly-carrier inline-C bodies (no `.fn`/
     `.env` on a scalar). Every Functor/Applicative/Monad instance body that
     calls the element fn via `fn.fn(...)` must be rewritten.
   - The raw-fn-pointer rewrite is NOT a safe substitute for `fn.fn` when a
     CAPTURING closure may be passed (it drops the env -> silent miscompile).
     fmap/bind/ap are routinely called with capturing closures, so those bodies
     must be rewritten to **pure-Turmeric `(fn x)`** (the probe form, which
     lowers thin/fat correctly), not to a raw-pointer inline-C cast.

2. **Most stdlib HKT instance bodies DELEGATE to an inline-C carrier helper**
   (`Comonad [pair]`'s `(extract [wa] (__pair_extract wa))`, Bifunctor
   [Result]'s `result-bimap`, etc.). A body that is a CALL to a global carrier
   helper is excluded by the by-value gate (`!is_global` is required), so it
   stays carrier. To go by-value the body must inline the work in pure Turmeric
   over the typed payload (`(.ok-val x)` etc.) -- which requires the underlying
   type to have pure-Turmeric field accessors (Option/Result/Either: yes;
   rc/identity/pair via Tuple2/built-ins: needs a pure accessor or stays
   carrier-essential).

3. **Per-class by-value readiness:**
   - **Foldable** -- DONE (sig typed; the sole instance `rc` is
     carrier-essential, stays inline-C; no body rewrite needed).
   - **Functor / Applicative / Monad / Alternative** -- the high-value targets.
     Option bodies are cleanly pure-Turmeric-rewritable and go by value
     (verified 2026-06-19: a probe Functor migration took `fmap (some 21) dbl`
     to a by-value `fmap_Option__spec`). **BUT `(Result _ B)` is BLOCKED by the
     two-param issue.** Attempting the by-value Functor migration with the
     `(definstance Functor [(Result _ B)])` instance present fails codegen with
     `__inst_Functor_fmap_Result__ltstruct_gt undeclared` -- the `_` wildcard /
     two-param head mangles to an anonymous `<struct>` under the by-value
     instance-spec path. So the SAME two-param-constructor gap reported for
     Bifunctor `bimap`
     (`docs/reported/m7-hkt-bimap-twoparam-struct-tyvar-leak.md`) is on the
     CRITICAL PATH for Functor/Monad/Applicative/Alternative too, because Result
     is an instance of all of them. **Resolving the two-param-constructor
     by-value handling is therefore the highest-leverage next compiler task**:
     it unblocks the Result instances across the four high-value classes at once.
     Also note: the element fn param must NOT be named `fn` in the by-value body
     (`fn` is the lambda keyword -> `(fn x)` misparses); use `g`/`func`.
     The RECURSIVE combinator instances (Parser, Goal, Backtrack, Schema) remain
     the other hard part -- they thread the `:fn` poly carrier through parser
     state and need careful pure-Turmeric rewrites (these broke the earlier
     unconditional-gate experiment, parent plan 3.1); rc stays carrier-essential.
     Sequencing per class: (1) land the two-param by-value fix, (2) upgrade sig +
     rewrite Option/Result/Either/list by-value, (3) rewrite-or-carrier the
     combinator instances, (4) regen snapshots, suite green.
   - **Comonad** -- `extract` is bare-element (hardenable), but `duplicate`
     returns the nested `(w (w a))` (the traverse blocker) and bodies delegate
     to carrier helpers; migrate `extract`'s sig per-method, keep extend/
     duplicate carrier until the nested-result emit lands.
   - **Bifunctor** -- blocked (two-param leak,
     `docs/reported/m7-hkt-bimap-twoparam-struct-tyvar-leak.md`); keep carrier.
   - **Traversable** -- 0 instances; blocked shape; nothing to migrate.
   - **MonadError** -- 2 instances (Result-family); assess per the Functor-family
     pattern.

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
