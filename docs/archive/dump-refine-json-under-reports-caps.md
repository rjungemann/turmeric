# `--dump-refine=json` under-reports which obligation hit a cap

**Severity:** low as a bug, medium as an instrument defect. Nothing miscompiles
and no verdict is wrong -- but the field is decision data for a future gate, and
it silently reads zero when it should read one.

**Status:** OPEN. Root-caused with a repro below, not fixed. Found while
measuring the `NO_MAX_SHARED` raise (solver-extension-plan SX5).

## Repro

```sh
TUR_REFINE_STATS=1 ./build/tur check \
    tests/fixtures/refine-match-field-wrong/input.tur --dump-refine=json 2>&1 \
  | grep -E 'caps_hit|NO shared|obligation\(s\)'
```

At `NO_MAX_SHARED` 8 (i.e. before the raise -- reproduce by reverting
`refine_solver.h` to `8`):

```
refine: 1 obligation(s): 0 proven, 0 refuted, 1 unknown (8 backend call(s), ...)
refine:   NO shared       peak      9 / 8      ** HIT
     "caps_hit": {},
```

One obligation in the file. The per-compile counters say a cap bit. That
obligation's `caps_hit` is empty.

## Root cause

`refine_discharge.c:559` snapshots the global cap counters, runs the chain, and
subtracts:

```c
const RefineCapStats caps_before = *refine_caps();
...                                     /* CHAIN[i](vc, a) for S0..S3 */
o->no_shared_hits = now->no_shared_hits - caps_before.no_shared_hits;
```

The subtraction is correct. The **window is too narrow**: `refine_discharge_one`
runs the full chain a second time, earlier, on a speculative probe -- RT4
template inference and path-splitting, at roughly `refine_discharge.c:450-465`
-- and that call happens *before* line 559.

The `8 backend call(s)` for a single obligation is the tell: `CHAIN_LEN` is 4,
so the chain ran twice. A cap that bites during the probe increments the global
counter outside the snapshot window, so the delta is 0 and the hit is
attributed to nobody.

## Why it matters

The SX8a notes call the per-obligation cap delta "the single most useful pairing
in the dump and exactly what SX6's gate would want per-site evidence from".
SX6's gate is a *start / do not start* decision on a 2-4 person-month phase. An
instrument that reports zero cap hits where there was one is the wrong direction
to be wrong in for that decision -- it argues for parking a phase that the
evidence might actually support.

The per-compile summary (`TUR_REFINE_STATS=1`) and
`benchmarks/run-cap-sweep.sh` read the global counters directly and are
**not** affected. Every SX0(b) number, and the `NO_MAX_SHARED` measurement that
found this, came from those. Only the JSON dump's per-obligation attribution
is wrong.

## Fix direction

Move the snapshot above the probe block, so the window covers all solver work
done on this obligation's behalf. That matches what the field claims to be --
`refine_collect.h:221` documents it as "caps this obligation alone hit".

The alternative reading, that the probe asks a different question and so its
caps are not this obligation's, argues for a second field
(`caps_hit_probe`) rather than for dropping the count on the floor. Either way
the current behaviour -- counted globally, attributed nowhere -- is the one
option that is not defensible.

Check `tests/fixtures/sx8a-refine-json-dump/expected.stdout` when fixing;
whether it moves depends on whether that fixture's obligations take the probe
path.

---

## Resolution (2026-08-26)

Fixed. A new `caps_hit_probe` field carries the probe work; nothing is dropped
on the floor any more.

### The root cause is right in substance, wrong in mechanism

The report locates the second chain run inside `refine_discharge_one` --
"`refine_discharge_one` runs the full chain a second time, earlier, on a
speculative probe ... and that call happens *before* line 559" -- and proposes
moving the snapshot above that probe block.

**That fix would have changed nothing.** The speculative branch
(`refine_discharge.c`, `if (ob->speculative)`) returns from its own `return`
long before the snapshot line; it never falls through to it. The probe is not
an earlier phase of this obligation's discharge -- it is a **different
`RefineObligation`, discharged in a different call, during elaboration**, from
`rt_prove_silent` (`elab_fns.c`). There is no snapshot in
`refine_discharge_one` that can be moved to cover it.

Confirmed by tracing the order on the report's own repro (`NO_MAX_SHARED`
reverted to 8):

```
TRACE: speculative discharge path_probe=1
TRACE: real discharge snapshot no_shared_hits=1
```

The global counter is already at 1 when the real obligation snapshots, so its
delta is 0. The `8 backend call(s)` the report reads as "the chain ran twice"
is correct; what it is not is two runs in one function.

A second thing the trace showed: the probe was a **path probe**, not RT4
template inference. `g_stats.path_probes` is incremented for it, but the stats
summary only prints that line `if (g_stats.proven_by_path)` -- so a path probe
that proves nothing is invisible in the summary too.

### What was done

The report's alternative reading is the one that survives contact with the
code, so `caps_hit_probe` it is:

- `RefineObligation` gains `caps_probe` alongside `caps`.
- `rt_try_prove_return` (`elab_fns.c`) brackets the whole `rt_prove_paths`
  call with a cap snapshot and hands the delta to the obligation it then
  collects. That function is the only place where "on whose behalf" is
  well-defined: the probes and the site's obligation are created a few lines
  apart, for the same predicate at the same location.
- The speculative branch of `refine_discharge_one` now snapshots and subtracts
  like the real branch, so a probe obligation records its own delta rather than
  leaving its work uncounted.
- `emit_caps` is parameterised on the key and called twice, so the record reads
  `"caps_hit": {...}, "caps_hit_probe": {...}`.
- The per-field subtraction (and a new hits-only accumulate) moved into
  `refine_caps_delta` / `refine_caps_add_hits` in `refine_solver.h`, since
  three call sites now need it. The peaks are still not differenced -- they are
  maxima, and the obligation records the peak its window SAW.

### Verification

The report's repro, with `NO_MAX_SHARED` temporarily back at 8:

```
before: refine: NO shared peak 9 / 8 ** HIT
        "caps_hit": {},
after:  refine: NO shared peak 9 / 8 ** HIT
        "caps_hit": {}, "caps_hit_probe": {"no_shared": 1},
```

Since the cap is 16 on `main` and that repro no longer bites, the regression
fixture (`tests/fixtures/refine-json-probe-caps-attributed`) uses a case that
caps at the shipped limits: a branching body (so path splitting is attempted)
under the `sx8a` fixture's 31-disequality predicate (so both the probes and the
whole-body obligation blow the cube cap). Against the pre-fix binary it reports
`caps_hit: {cubes: 4}` with four further hits attributed to nobody; after, both
fours are reported and are reported separately.

`tests/fixtures/sx8a-refine-json-dump/expected.stdout` did not move -- the
report flagged it as a maybe, and its obligations do not take the probe path.
Full suite: 2704 passed, 0 failed.

`docs/guides/refinement-solver-internals-guide.md` documents both fields and
when to sum them.
