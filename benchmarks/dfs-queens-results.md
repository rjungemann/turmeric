# SX2 driver vs the list monad -- N-queens, both shipped surfaces

`benchmarks/bench-dfs-queens.tur`, Release-built `tur`, both paths counting all
solutions.  Three runs; figures below are run 1 and the spread across runs is
under 10%.  (A first attempt measured a STALE `build-release/tur` that
predated two landed fixes and silently produced wrong answers -- rebuild
Release before believing any number from it.)

| N | solutions | list ns/run | dfs ns/run | dfs / list |
|---:|---:|---:|---:|---:|
| 4 | 2  | 6,480   | 7,179   | 1.11x |
| 5 | 10 | 20,875  | 21,654  | 1.04x |
| 6 | 4  | 76,954  | 84,174  | 1.09x |
| 7 | 40 | 310,813 | 329,889 | 1.06x |

**There is no crossover in the measurable range: the list monad is 4-11%
ahead everywhere.**  The plan's SX2 gate measured the trail 11-34x ahead of
the PERSISTENT SUBSTITUTION (`bench-logic-subst`), and both results are true
at once: that benchmark measured lookup against `logic.tur`'s `SBind` chain,
while this one measures full search enumeration, where the list monad's inner
loop is a tight inline-C cell walk and the driver pays a fat-closure dispatch
per combinator layer per node.  The trail's win is against persistent-state
LOOKUP, not against list-based ENUMERATION at small N.

**What the driver buys instead is scale and reification.**  The list path
threads the board through the monad as a packed int, 3 bits per column --
structurally capped at N = 7.  The driver's board lives in cells, so it keeps
going (dfs-only, same build):

| N | solutions | dfs ns/run |
|---:|---:|---:|
| 8  | 92  | 1,411,860 |
| 9  | 352 | 6,529,803 |
| 10 | 724 | 35,294,621 |

All counts match the known sequence (2, 10, 4, 40, 92, 352, 724).  And
solutions are reified at success time from live cells -- the list path would
have to widen its packing or switch representations entirely to report an
N = 10 placement, where the driver's `on-solution` just reads the cells.

Neither path is hand-tuned; each is the straightforward program its surface
invites.  Both leak their per-run structures (the monad's cells are never
freed; the driver's board cells are allocated per run) -- equal footing, and
not what was being measured.
