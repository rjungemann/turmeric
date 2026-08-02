# Dogfooding `refined` on tur-ecs -- the report

**Date:** 2026-07-26. **Program:** `turmeric-spices/spices/ecs` -- ~5400 lines
across 22 modules, 66-test suite, 66/66 green, carrying the shipped RE1
refined surface (`ecs/refined-world`, `for-each-alive!`, 8 refined test
files). This is the report `../archive/refined-dogfooding-plan.md` specifies; it
feeds `refined-graduation-plan.md` precondition 2 (cost on a real program) and
the Z3 retirement decision.

**Method:** Release `tur` (current `refinements` head). Per file x config,
3 reps of `tur check`, min wall time; configs = gate off / `--enable=refined`
/ `--enable=refined --strict-refine`. Every test-file compile is a real
multi-module elaboration (each pulls the spice's module tree). Corpus split:
58 plain files (no refinements anywhere -- the "tax on unannotated code"
population) + 8 refined files (`#lang turmeric refined`; gate effectively
always on for them).

## The two questions

**1. Does `TUR-E0371` fire on correct code? -- No. Zero E0371 across the
corpus, all three configs.** Nothing fired, so no false positive exists to
weigh; note the corpus also contains no definitely-wrong literal call, so this
is an absence-of-false-positives result, not a stress of the closedness
widening. `TUR-W0372` = 4, and all four are the INTENTIONAL negative tests
(no-region / no-guard / wrong-entity), each expected by its
`tur-test-expect-error` directive. `TUR-W0377` = 0. No diagnostic on any
correct file.

**2. What does it cost? -- Noise.**

| corpus | off | on | strict | ratio on/off | ratio strict/off |
|---|---|---|---|---|---|
| 58 plain files (sum of per-file min) | 0.61s | 0.61s | 0.61s | **1.004** | **1.001** |
| 8 refined files | 0.12s | 0.12s | 0.11s | n/a (always on) | -- |

Worst single-file delta on/off: **+1ms**. `tur build` spot checks
(spawn1k-wide, xworld-3world, refined-foreach-alive) show no measurable
gate delta on the build path either. The fixtures' "1.7x on a
crossing-heavy file" number does not transfer to a real program: real
refinement density is 8 obligations across 5400 lines, each costing 21-30
backend calls, and unannotated modules pay ~0.4%. Caveat for the record: the
corpus is many small compiles (11-42ms each), not one monolithic build --
but each IS a full multi-module elaboration, and the tax scaled with none of
them.

**Obligations (strict):** 8 files, 1 obligation each -- 4 proven (the
positive aliveness/loop/module/foreach tests), 4 unknown BY DESIGN (the
negative tests). 0 refuted, 0 dropped-before-solver. Memo hits 0 (per-file
processes; the memo is per-process and this corpus gives it nothing to reuse).

## Second round (same day, denser corpus)

After the RE1 promotion landed (`ecs/sized-refined`; corpus now 70 files, 12
refined, including macro-EMITTED refined accessors and two more intentional
negatives): plain-corpus ratio **1.015** on / **1.012** strict; `TUR-E0371`
still zero everywhere; `TUR-W0372` exactly the five intentional negatives;
`TUR-W0377` still zero. The added surface did not move the cost or produce a
false positive. (Procedural note for the record: the first re-run used a
stale Release binary predating the template-emitter fixes and silently
mis-scored the new negative -- rebuild before measuring; the figures above
are from the rebuilt binary.)

## The Z3 oracle cross-check (retirement evidence -- run while it still exists)

Oracle build: `-DTUR_REFINE_Z3_ORACLE=ON` against system Z3 4.15.4 (links
`libz3.4.15.dylib`; Debug, sanitizers off). All 8 refined files re-checked
under the oracle: **verdicts identical to the in-house chain (4 proven / 4
unknown), zero `TUR-I0379`** -- no VC where the in-house stage claimed
`RT_VALID` and Z3 disagreed. This is the "real VCs, not corpus VCs" evidence
the Z3 retirement decision asked for, banked before the scaffold is deleted.

Extended the same day: oracle rebuilt at head and re-run over all 12 refined
files including the `ecs/sized-refined` promotion surface (macro-emitted
measures + refined accessors -- novel crossing shapes the first run
predates): still zero `TUR-I0379`.

## Tier coverage (what the program supplied naturally; forced nothing)

- **T1 guarded calls (if/let/match, guard several branches up, value rebound
  by a `let`)** -- COVERED, heavily: every refined test is this; the
  `for-each-alive!` macro's expansion is guard-above-rebinding by
  construction.
- **T1 literal-to-refined-callee closedness** -- the negative direction only
  (no E0371 on correct code); the corpus has no wrong literal call to trip
  it. Absence result.
- **T1 typeclass refined-method variance** -- SKIPPED: the ecs classes
  (`Component`, `StorageOps`) have no refined method parameters; nothing
  natural to write. (RM-B1 fixtures cover it synthetically.)
- **T2 match-with-refined-arms, :pre/:post, float refinements, mutual
  recursion** -- SKIPPED: no natural site. Single recursion IS covered
  (the refined loop's inductive lower bound).
- **T2 stdlib aliases (`Nat`/`Pos`/`Byte`/`Percent`)** -- SKIPPED, and this
  is a finding: the refinements real ECS code wanted were **stateful
  measures** (`alive?`, `in-bounds?`) and bounds-on-`Slot`, none of which the
  arithmetic alias layer expresses. The stdlib layer is aimed at a different
  register than the first real consumer reached for.
- **T3 `while` accumulating into a refined value** -- CONFIRMED as the
  documented limit, from the demand side: hit twice while building RE1 (c)
  (the `set!` accumulator body; the `while` lowering), worked around with the
  recursion shape both times. The trigger condition in
  `hold/loop-invariants-plan.md` ("a real program wants it") is technically
  met, but the recursion shape + the planned frame-aware invalidation
  (`checked-write-frames-plan.md` WF3) cover the actual demand better than
  hand-written invariants would.
- **T3 refined fn-values, nonlinear predicates** -- SKIPPED: no natural site.

## Guide accuracy

Nothing newly inaccurate found this round (the RE3 pass had just reconciled
the guides; the limits behaved exactly as documented, including the
whole-body `set!` decline and the impure-measure no-runtime-fallback
message).

## What this changes

- Graduation precondition 2 can move to **measured on a real program:
  ~1.004x on unannotated code, no false positives** (this report).
- The Z3 oracle agreement on real VCs is banked for the retirement decision.
- Annotation-burden reading: the real program adopted refinements where they
  said something true (aliveness, bounds) and the signatures needed the
  stateful machinery, not the arithmetic aliases -- evidence for keeping the
  `#reads`/`frozen` track first-class in the graduation story.
