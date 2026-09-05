# The refinement chain expands the identical DNF once per stage

**RESOLVED 2026-09-05.** Built once per chain run, in all THREE drivers plus the
`tur smt` and wasm doors. Measured after: the heaviest in-tree fixture goes
48 builds -> 13, and a corpus benchmark reaching S3 goes 4 -> 1. Verdicts
identical on all 125 corpus benchmarks and all 45 refine fixtures, and
`TUR_REFINE_STATS` identical across both populations. See **Resolution** at the
end -- including one thing the report's own measurement table got wrong.

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

---

# Resolution, 2026-09-05

## What landed

`RefineCubeCache` -- a plain struct holding `{tried, ok, VCCubeSet}` -- is
declared once per chain run and threaded through `refine_sN_decide_cc`
variants. `refine_cubes_get` builds on the first ask and hands back the same
set thereafter. The four `refine_sN_decide` entry points remain as thin
wrappers over the `_cc` ones with a fresh cache, so a single-stage caller is
byte-identical.

Two deliberate departures from the fix direction above:

**LAZY, not build-once-up-front.** The direction says "build once in the chain
driver and pass it down". Doing that literally would have been a REGRESSION on
the cheapest path: S0 decides several shapes syntactically (`hyp_contains`, ex
falso) *before* it needs cubes, and those obligations build zero today. An eager
driver build makes them pay one. Building on first ask keeps 0 at 0 and turns 4
into 1. This is still not a cache on the `RefineVC` -- it is a local with the
lifetime of one chain run, so the `(pop)` staleness trap the report warns about
does not arise and no generation counter is needed.

**The `tur smt` and wasm doors got it too.** The direction says to leave them
unchanged. Their ENTRY POINTS are unchanged -- that is what matters for
`tests/unit/refine_solver.c` and the SX8 doors -- but `smt_answer` (main.c) and
`smt_one_answer` (wasm_glue.c) each run their own copy of the same four-stage
loop, with the same duplication. `smt_answer`'s own comment says it runs "the
same chain, in the same order, as the compile path -- running a different one
here would make this window show something other than the solver it is a window
onto", which is an argument for changing it, not against.

That mattered for the measurement, below.

## The report's table measures two different drivers

The table presents four rows as one finding. They are not: the three corpus
rows come from `tur smt`'s loop in `main.c`, the fixture row from
`refine_discharge.c`'s chain. Fixing only the discharge chain (the literal fix
direction) left the corpus rows at 4, 4, 3 -- unchanged -- which is what
exposed the conflation:

| unit | before | after discharge only | after all drivers |
|---|---:|---:|---:|
| `qf_lra_ite_int_numerals_unsat` | 4 | 4 | **1** |
| `qf_lia_ite_nested_sat` | 4 | 4 | **1** |
| `qf_lia_ite_two_distinct_unsat` | 3 | 3 | **1** |
| `refine-crossing-path-conditions` | 48 | 13 | **13** |

So "75% of the cube expansion in this solver is duplicate work" was right about
the quantity and wrong about the scope -- it was duplicate work in three
independent loops, and a fix to one of them would have left the report's own
headline benchmarks untouched while appearing to close it.

## Acceptance

The report's own criterion, run:

- **125 corpus benchmarks** -- verdicts byte-identical (`tur smt`, before vs
  after, two confirmed-different binaries).
- **45 refine fixtures** -- output byte-identical.
- **`TUR_REFINE_STATS`** -- identical across the 45 top-level corpus units and
  four refine fixtures, cap telemetry included.

One fixture DID move, exactly as the report predicted:
`refine-json-probe-caps-attributed`'s `cubes` hit counts go 4 -> 1 in both the
own and probe windows. That counter counts BUILDS that hit the cap, and there is
one build now. The cap still hits, both windows are still attributed, and
own/probe stay separate -- which is everything the fixture exists to protect.
Its expected output is updated and the hook records why, so the next reader does
not mistake it for a regression.

`benchmarks/cap-sweep-results.md` was regenerated as the report asks. **Nothing
moved but the provenance line** -- no unit in the corpus, in-tree or fuzzer
populations caps on `cubes` at all (the one capping unit caps on `euf_terms`),
so the predicted hit-count churn is confined to the hook fixture above. That
non-result is the reason to have run it.
