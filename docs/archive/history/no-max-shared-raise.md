---
title: Raising NO_MAX_SHARED from 8 to 16
category: History
description: The one refinement-solver cap with a live signal, raised on SX0(b)'s evidence -- what was measured before landing it, and the two interactions that made it not-quite-free.
---

# Raising `NO_MAX_SHARED` from 8 to 16

**Landed 2026-08-25.** `src/compiler/refine_solver.h:56`.

The Nelson-Oppen stage (S3) exchanges entailed equalities between EUF and
linear arithmetic over the terms both theories can see. `NO_MAX_SHARED` bounds
how many of those shared terms it will consider; over the cap it simply does
not combine, which is `RT_UNKNOWN`, which is sound but incomplete.

## Why this cap and not another

SX0(b)'s sweep found it was the **only cap in the solver with a live signal**.
Across 411 units (124 corpus benchmarks, 87 in-tree refinement fixtures, 200
fuzzer-generated VCs), every other cap sat on 72-98% headroom and fired either
never or only on a known artifact:

| cap | peak / limit before | hits |
|---|---:|---|
| `REFINE_MAX_LA_CONSTR` | 42 / 512 | none, anywhere |
| `REFINE_MAX_CUBES` | 40 / 64 | none, anywhere |
| `REFINE_MAX_CUBE_LITS` | 13 / 64 | none, anywhere |
| `REFINE_MAX_EXPAND_DEPTH` | 8 / 256 | none, anywhere |
| `REFINE_MAX_LA_VARS` | 9 / 32 | none, anywhere |
| `REFINE_MAX_EUF_TERMS` | 512 / 512 | 982, all on one regression artifact |
| `NO_MAX_ROUNDS` | -- | none, anywhere |
| **`NO_MAX_SHARED`** | **9 / 8** | **4 units, always by exactly 1** |

Four units had 9 eligible shared terms against a cap of 8 -- over by one, every
time. The fuzzer's own peak was 7 of 8, so the distribution sits right against
the limit rather than far below it.

## Why it was not a drive-by

The plan (solver-extension-plan SX5) flagged the cost, and the flag was right.
The exchange in `no_cube_unsat` is **quadratic in the shared set**, and both of
its inner loops call `la_entails_eq`, which runs Fourier-Motzkin twice. So
8 -> 16 is **4x the pair work on every S3 cube** -- and that is paid by every
obligation that reaches S3, not only by the ones that were near the cap.

There is a second interaction the pair-count does not show: more shared terms
means more equalities to discover, which could push the exchange into
`NO_MAX_ROUNDS` (4) and trade one cap hit for another. That had to be checked,
not assumed.

## What was measured before landing it

Release build (`build-release`), best of three timed rounds after an untimed
warm-up, per the SX0(a) method note. Verdicts captured from the same binaries
that were timed.

**Verdicts -- nothing moved.**

| population | before (cap 8) | after (cap 16) |
|---|---|---|
| SMT-LIB corpus, 125 benchmarks | 68 proved, 56 sat-correct, 1 skipped, 0 soundness failures | identical |
| corpus, per-benchmark diff | -- | **byte-identical, all 125 lines** |
| in-tree refinement fixtures, 89 | per-fixture proven/refuted/unknown | **identical, all 89** |
| `bash tests/run.sh` | 2694 passed, 0 failed | 2694 passed, 0 failed |

**Cost -- not measurable.**

| workload | cap 8 | cap 16 |
|---|---:|---:|
| corpus replay (125 benchmarks) | 0.0926 s | 0.0923 s |
| 89 refinement fixtures, `tur check` | 1.566 s | 1.552 s |

Both moved *down* by less than half a percent, which is noise -- the 4x is 4x
of a term that does not dominate either workload. The fixture number in
particular is mostly process startup (~17 ms/fixture), which is why the corpus
is the sharper of the two instruments here.

**The round budget was not traded into.** `refine-match-field-wrong`, the
in-tree unit that hit the cap, now reports `NO shared peak 9 / 16` with no hit
and `NO rounds out 0 (of 4 rounds)` -- the exchange reached its fixpoint inside
the budget with the wider set.

**Cap telemetry after.** `benchmarks/cap-sweep-results.md` regenerated: 
`no_shared` hits drop to **0 across all three populations**, with 44-56%
headroom. The only capped unit left anywhere is
`qf_lra_deep_arith_chain_sat.smt2` on `euf_terms`, which is the 1000-deep
`(+ 1 (+ 1 ...))` stack-overflow regression -- an artifact of the regression,
not a shape real code produces.

## What this did not buy

**No new proofs.** All four units that hit the cap were ones that must answer
`UNKNOWN` regardless -- three generated corpus benchmarks labelled `sat`, and
`refine-match-field-wrong`, a soundness fixture whose obligation is a genuine
violation. SX0(b) already said this ("not one cap hit cost a proof"), and
raising the cap confirms it: the verdicts are identical.

So this is headroom, not capability. It was worth taking because the
distribution was sitting one term over a limit set by caution rather than by
evidence, and because the measurement to justify it is the same measurement
SX5 would have to run anyway.

## Note on the sweep's unit counts

The regenerated sweep reports 85 in-tree units where the 2026-08-22 run
reported 87. That is a difference between commits `2b5e3572` and `aeb87927`,
not between cap 8 and cap 16 -- the before/after verdict diff above was taken
from one tree with only the constant changed, and covers 89 fixtures with zero
differences.
