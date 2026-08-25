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
