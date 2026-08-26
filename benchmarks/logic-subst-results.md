# SX2: trailed substitution vs persistent assoc list

Generated: 2026-08-22 18:32 at 8c987b89 by `benchmarks/bench-logic-subst.tur`.

The solver-extension plan's honest test: if a trailed substitution does not
beat a persistent association list, the trail primitive is not paying for
itself and SX3/SX4 should proceed as plain C with no shared utility.

Both paths run the SAME workload -- bind n variables, walk all n, backtrack --
and their results are compared, so neither loop can be optimized away and a
divergence would surface as a wrong answer rather than a faster one.

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

**Verdict: the trail pays for itself, and there is no crossover.** It wins at
every size measured, from 12x at a single binding to 36x at 512.

The shape is the interesting part. A persistent list is O(1) to extend and free
to backtrack but O(n) to look up, so the expectation was that it would win at
small n and lose at large n. It never wins. Its cost is roughly FLAT at
190-370 ns/op from n=1 to n=64 and only starts climbing past n=128 --
which means a large per-operation constant, not the linear scan, is what
dominates at every size a real query reaches. The asymptotics only take over
at the very top of the sweep.

That constant is worth understanding on its own account; see
`docs/reported/solver-hot-structures-linear-scans.md`. It is not diagnosed
here -- a microbenchmark that tried to isolate it was optimized away entirely
(the same trap as SX0(a)'s closure baseline), and a number from a folded loop
is worse than no number.
