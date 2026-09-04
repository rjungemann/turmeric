# The refinement chain expands the identical DNF once per stage

**Severity:** low. Not a correctness bug and not, on today's evidence, a
performance one -- the whole solver is compile-time noise (21 vs 22 ms/check
solver-on against solver-off on the heaviest fixture, SX0(b)). Filed because it
is a structural redundancy that is cheap to remove and because the measurement
that found it also retires a planned phase.

**Status:** OPEN, found 2026-09-03 while scoping SX6a step zero.

## What happens

`refine_discharge.c`'s `CHAIN[]` runs S0 -> S1 -> S2 -> S3 in order, stopping at
the first stage that decides. Each stage independently opens with:

```c
VCCubeSet cs;
if (!refine_cubes_build(vc, a, &cs)) return refine_unknown();
```

-- `refine_solver_s0.c:67`, `refine_solver_euf.c:264`, `refine_solver_arith.c:443`,
`refine_solver_no.c:114`. The VC is unchanged between stages, so all four builds
run the same NNF conversion and the same DNF product expansion and produce the
same cube set. An obligation that reaches S3 pays for it four times; three of
the four are discarded.

## Measurement

A temporary counter in `refine_cubes_build` / `expand`, over `tur smt` on the
corpus and `tur check` on the in-tree fixtures:

| unit | builds | cubes built (sum) | cubes per build (peak) | expand frames |
|---|---:|---:|---:|---:|
| `qf_lra_ite_int_numerals_unsat` | 4 | 64 | 16 | 124 |
| `qf_lia_ite_nested_sat` | 4 | 64 | 16 | 124 |
| `qf_lia_ite_two_distinct_unsat` | 3 | 48 | 16 | 93 |
| `refine-crossing-path-conditions` (heaviest in-tree) | 48 | 71 | 4 | 117 |

The `cubes` column is a SUM across builds and the `peak` column is the widest
single build, so the first row reads: one 16-cube DNF, expanded four times.
**75% of the cube expansion in this solver is duplicate work.**

## Why this is filed rather than fixed

The payoff is noise by the solver's own telemetry, so it does not clear the bar
on its own. It is worth doing when something else touches this code, or as a
standalone simplification if someone wants the chain to read more honestly.

## Fix direction, and the trap to avoid

**Do not cache the cube set on the `RefineVC`.** That is the obvious move and it
is now unsound: SX8b's `(pop)` truncates `vc->n_hyps` directly, so a cache keyed
on `(n_hyps, goal)` can be stale after `pop` + re-`assert` re-grows the count to
the same value with different hypotheses. Invalidating it correctly needs a
generation counter bumped on every mutation of the hypothesis array, which is a
soundness-critical invariant added for a noise-level win.

**Build once in the chain driver and pass it down.** Add
`refine_sN_decide_cs(vc, a, const VCCubeSet *)` for each stage, keep the
existing `refine_sN_decide` as a thin wrapper that builds and delegates (so
`tur smt`, `tests/unit/refine_solver.c` and the SX8a/SX8b doors are unchanged),
and have `refine_discharge.c`'s chain build the set once and call the `_cs`
variants. No cache, no invalidation, no new invariant -- the cube set is a local
with the lifetime of one chain run, and the result is verdict-identical by
construction.

Acceptance is the SX3 pattern: identical verdicts on all 125 corpus benchmarks
and every `refine-*` fixture, and identical `TUR_REFINE_STATS` counts. Note the
cap telemetry WILL move and should be expected to: `cubes_hits` /
`cubes_peak` are recorded per build, so building once instead of four times
changes the hit counts on any unit that was capping, and
`benchmarks/cap-sweep-results.md` needs regenerating in the same commit.

## What this measurement retires

SX6a's "step zero" in the solver-extension plan is *stream cubes off an explicit
stack instead of materializing up to 64*. The premise does not hold: nothing
materializes anything near 64. The widest single cube set anywhere measured is
**16**, on three corpus benchmarks; the heaviest in-tree refinement fixture
peaks at 4. There is no array worth not building, and streaming would save
neither memory nor time. See the plan's SX6a section for the retirement note.
