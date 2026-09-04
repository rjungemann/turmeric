# SX2: trailed substitution vs persistent assoc list

Re-generated: 2026-09-03 at caf22bc5 (v0.43.0) by `benchmarks/bench-logic-subst.tur`,
Release build, 4-core Xeon @ 2.10 GHz. Medians of three runs.

**The original run (2026-08-22 at 8c987b89) is preserved at the bottom.** It
predates SR1, SR2a/b, SR3 and the SR4 default flip -- every representation
change that touches this benchmark's subject -- so it is kept as the "before"
column rather than overwritten silently.

The solver-extension plan's honest test: if a trailed substitution does not
beat a persistent association list, the trail primitive is not paying for
itself and SX3/SX4 should proceed as plain C with no shared utility.

Both paths run the SAME workload -- bind n variables, walk all n, backtrack --
and their results are compared, so neither loop can be optimized away and a
divergence would surface as a wrong answer rather than a faster one.

## The gate verdict, re-run

| bindings | persistent ns/op | trailed ns/op | speedup |
|---:|---:|---:|---:|
| 1 | 112.8 | 9.9 | 11.4x |
| 2 | 120.3 | 9.5 | 12.7x |
| 4 | 119.5 | 7.9 | 15.1x |
| 8 | 132.6 | 9.5 | 14.0x |
| 16 | 169.6 | 7.7 | 22.1x |
| 32 | 239.3 | 7.0 | 34.1x |
| 64 | 419.2 | 7.2 | 58.3x |
| 128 | 783.7 | 9.0 | 87.5x |
| 256 | 1595.8 | 16.6 | 95.9x |
| 512 | 3297.7 | 15.3 | **215x** |

**The verdict is unchanged and the margin has widened by an order of
magnitude** -- 11x to 215x, against the original run's 11x to 34x. There is
still no crossover.

**But the SHAPE has changed, and the shape was the interesting part.** The
original run found the persistent path roughly FLAT at 190-370 ns/op from
n=1 to n=64, climbing only past n=128, and concluded that "a large
per-operation constant, not the linear scan, is what dominates at every size a
real query reaches". That conclusion was correct then and is **wrong now**:
the curve is flat only to n=4 and is cleanly linear from n=8 up (2.0x per
doubling from n=64). The constant that used to mask the scan was the
allocator, and SR1/SR2/SR4 have taken most of it away; what is left is the
`SBind` chain walk itself.

## Why the persistent path got FASTER at small n and SLOWER at large n

The two halves have different causes, so the sweep was re-run against the
representation hatches. `TUR_SR4_RECURSIVE_CARRIER=1` restores the pre-flip
carrier for `Term`/`Subst`/`Stream`; `TUR_SR1_SUM_BYVALUE=0` additionally
restores it for `Lookup`/`UnifyResult`.

| bindings | default (SR4 by value) | `TUR_SR4_RECURSIVE_CARRIER=1` | by value / carrier |
|---:|---:|---:|---:|
| 1 | 112.8 | 109.7 | 1.03x |
| 2 | 120.3 | 105.4 | 1.14x |
| 4 | 119.5 | 95.3 | 1.25x |
| 8 | 132.6 | 95.2 | 1.39x |
| 16 | 169.6 | 92.4 | 1.84x |
| 32 | 239.3 | 111.1 | 2.15x |
| 64 | 419.2 | 143.4 | 2.92x |
| 128 | 783.7 | 196.3 | 3.99x |
| 256 | 1595.8 | 283.5 | 5.63x |
| 512 | 3297.7 | 486.7 | **6.78x** |

Adding `TUR_SR1_SUM_BYVALUE=0` on top moves nothing (138 / 121 / 112 / 115 /
112 / 139 / 158 / 201 / 286 / 503 ns/op, within run-to-run noise of the column
above), which is the control this benchmark should have: `logic.tur`'s hot
types are all self-recursive, so SR1 is structurally invisible here and SR4 is
the entire effect.

So both halves of the change are SR4's:

- **At n<=4 the by-value flip is at parity or slightly ahead** -- fewer mallocs
  per bind, which is what RM4 measured.
- **From n=8 it loses, and the loss grows without bound** -- the chain walk
  now copies aggregates where it used to move a pointer.

The mechanism is visible in the emitted C. `sizeof(tur_adt_Term)` is 24 and
`sizeof(tur_adt_Subst)` is 48, and `subst_hylookup`'s recursive arm reads:

```c
int64_t v_1597       = (int64_t)__scrut->as.SBind._0;
tur_adt_Term term_1598 = __scrut->as.SBind._1;                              /* 24B, dead on the miss path */
tur_adt_Subst rest_1599 = *(tur_adt_Subst *)(intptr_t)(__scrut->as.SBind._2); /* 48B load+copy */
...
tur_adt_Subst __t310 = rest_1599;                                            /* 48B copy, AGAIN */
tur_adt_Lookup __ps_311 = (subst_hylookup(vid, &__t310));                    /* then take its address */
```

120 bytes copied per chain link, of which the 24-byte `Term` is dead whenever
the arm takes the recursive branch and the second 48-byte copy is redundant
with the first. The carrier path passes one `int64_t`. Filed as
[docs/reported/sr4-byvalue-recursive-sum-walk-copies-per-link.md](../docs/reported/sr4-byvalue-recursive-sum-walk-copies-per-link.md).

**This does not reverse SX2's gate** -- the trail wins either way, and wins by
more under the default. It does mean the persistent column is now measuring a
representation choice as much as a data-structure choice, which is worth
knowing before quoting the 215x anywhere.

## Original run, 2026-08-22 at 8c987b89 (pre-SR)

| bindings | persistent ns/op | trailed ns/op | speedup |
|---:|---:|---:|---:|
| 1 | 209.5 | 18.7 | 11.2x |
| 2 | 210.9 | 14.0 | 15.0x |
| 4 | 232.6 | 12.1 | 19.3x |
| 8 | 194.1 | 11.5 | 16.8x |
| 16 | 189.9 | 11.5 | 16.5x |
| 32 | 293.2 | 18.5 | 15.8x |
| 64 | 369.9 | 20.7 | 17.9x |
| 128 | 470.6 | 27.8 | 17.0x |
| 256 | 385.6 | 21.3 | 18.1x |
| 512 | 782.2 | 22.7 | 34.4x |

Different box, so the absolute numbers do not compare across the two tables --
which is exactly why the representation A/B above was run on ONE box with one
compiler and one binary per arm, rather than by differencing these two tables.

## A note for the next person to run this

The benchmark had bit-rotted and would not build: `tur_trail_mark_packed`,
`tur_trail_undo_to_packed` and `tur_trail_reset` are compiler-predeclared now,
so the file's own `extern-c` rows for them were a hard "already defined" error.
Fixed in the same commit as this re-run. Nothing in `tests/` or `ctest` builds
this file, which is why the rot went unnoticed for a release cycle.
