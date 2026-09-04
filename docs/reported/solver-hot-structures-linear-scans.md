# The refinement solver's hot structures are linear scans

**Severity:** low today, structural. Every one of these is inside a cap, so none
of them can hang a compile -- they make the caps bite sooner than they need to,
which costs completeness rather than time. Worth fixing when the surrounding
phase touches the code anyway; not worth a standalone project.

**Status:** OPEN, and reassessed 2026-08-27 with SX3 landed and measurements
in hand. Two corrections to this report's own claims:

- **The "free fix with SX3" prediction did not come true.** SX3 landed as
  mark/undo over the SAME arrays (`trail_c.h` value-trails `parent[]`); it did
  not rewrite the term index, so #1's linear scan survived the phase that was
  supposed to absorb it. Fixing it is now a standalone change again.
- **And the measurements say that standalone change buys nothing.** Real
  obligations peak at **10 of 512** EUF terms (heaviest in-tree fixture, via
  `TUR_REFINE_STATS`); across all 125 corpus benchmarks exactly one reaches
  the 512 cap -- a deliberate 1000-deep stress regression -- and it decides in
  **64 ms**. Solver-on vs solver-off (`TUR_REFINE_NO_DISCHARGE=1`) on the
  heaviest fixture is 21 vs 22 ms/check: the whole solver is compile-time
  noise. The post-SX3 cap-sweep re-run is byte-identical to the pre-SX3
  baseline except the timestamp.

So the scans stay filed (the scaling property is real), but nothing here
justifies work until an obligation population with hundreds of terms exists --
and SX0(b)'s telemetry will show it arriving before it hurts.

## 1. `euf_index` interns terms by linear scan -- O(n^2) in term count

`src/compiler/refine_solver_euf.c:63`

```c
static uint32_t euf_index(EufState *st, VCTerm *t) {
    if (!t) return UINT32_MAX;
    for (uint32_t i = 0; i < st->n; i++) if (st->terms[i] == t) return i;   /* <-- */
    ...
```

Every subterm registration scans the whole term array. Registering `n` terms is
O(n^2) pointer compares, and `REFINE_MAX_EUF_TERMS` is 512, so the worst case is
~131k compares per EUF state -- rebuilt per cube today, up to 64 cubes.

The lookup is by POINTER identity on a hash-consed `VCTerm`, so a pointer-keyed
hash table drops it to O(1) amortized with no change in semantics. `euf_equal`
(line 96) has the same scan.

**Where the fix belongs:** this section originally said SX3, which "rewrites
this state to carry mark/undo anyway". SX3 landed WITHOUT rewriting the index
(mark/undo trails the existing arrays in place), so that home is gone; see the
status note above for why no new home is needed yet. If it is ever done, note
the index must now also be trail-aware: undo truncates `n`, which a plain hash
table over dead entries would not survive.

## 2. The congruence-closure fixpoint is a naive O(n^2) sweep

`src/compiler/refine_solver_euf.c:117,131` -- re-scan all pairs until nothing
changes. The file header says so explicitly and calls it the right tradeoff at
this size, which it is. Recorded here only so the two scans are not confused:
this one is the ALGORITHM (and fixing it means Nieuwenhuis-Oliveras, i.e. SX3
proper), while #1 is an implementation detail with a free fix.

## 3. `collect_shared` scans every EUF term per S3 cube

`src/compiler/refine_solver_no.c:29`. It always did; SX0(b) made it scan past
the cap too, to count what it discards for the telemetry. Bounded by
`REFINE_MAX_EUF_TERMS` (512) per cube, so ~32k iterations of a cheap predicate
in the worst case, and only on obligations that reach S3. Called out for
completeness, not because it is worth changing on its own.

## Related, and being addressed

`stdlib/logic.tur`'s `Subst` is a persistent association list
(`SBind :int :Term :Subst`) walked linearly by `subst-lookup`, so unification
over `n` bindings is O(n^2) and `logic-walk` is O(n) per variable. This is
exactly what SX2 of the solver-extension plan exists to measure a replacement
for -- see that phase rather than treating it as a separate finding.

## 4. The persistent-structure per-operation constant (~200 ns) -- DIAGNOSED, then SUPERSEDED

> **Superseded 2026-09-03.** The conclusion below -- "a per-operation constant
> rather than the linear scan dominates at every size a real query reaches" --
> was true of the compiler that measured it and is **no longer true of the
> default**. Re-running `benchmarks/bench-logic-subst.tur` at v0.43.0 finds the
> persistent path flat only to n=4 and cleanly LINEAR from n=8 up (2.0x per
> doubling from n=64, reaching 3298 ns/op at n=512). The allocator constant that
> used to mask the scan is largely gone -- SR1/SR2/SR4 removed it -- so on
> today's default the chain walk itself is what dominates, which is the opposite
> of what this section concludes.
>
> Two consequences, and neither one revives the term-index work in #1 above:
>
> - The linear scan that now dominates is `subst-lookup`'s walk of a persistent
>   `SBind` chain in `stdlib/logic.tur` -- a stdlib data structure -- not
>   `euf_index`'s term scan in the compiler's own C. #1, #2 and #3 are in
>   `src/compiler/refine_solver_*.c`, which no representation change touches;
>   their status note above stands unaltered.
> - Most of the new slope is not the scan's asymptotics either. It is a
>   per-link aggregate copy the by-value recursive-sum lowering emits, worth
>   6.8x at n=512 against `TUR_SR4_RECURSIVE_CARRIER=1` and filed separately as
>   [sr4-byvalue-recursive-sum-walk-copies-per-link](sr4-byvalue-recursive-sum-walk-copies-per-link.md).
>
> Numbers, method and the representation A/B are in
> [benchmarks/logic-subst-results.md](../../benchmarks/logic-subst-results.md).
> The original diagnosis follows, unedited.

SX2's head-to-head measured `stdlib/logic.tur`'s real `Subst` at ~175-230 ns per
bind+walk, flat in n up to n=16, which said a per-operation constant rather than
the linear scan of #1 dominates at every size a real query reaches.

**That constant is the allocator, and the finding is language-wide rather than
solver-specific**, so it moved to its own report:
[multi-variant-adts-always-heap-allocate.md](multi-variant-adts-always-heap-allocate.md).

In short: ~85% of executed instructions on that workload are inside `malloc`,
because a multi-variant ADT heap-allocates on every construction however small
it is (single-variant ones already lower by value). It is not the `O(n)` scan,
and fixing the scan would not have helped. The cheapest fix -- a slab allocator
for ADT boxes, needing no ownership analysis -- prices at 2.4x there.

## Context

Turmeric has not had a deliberate performance pass. These are not regressions;
they are first-implementation choices that were correct when the structures were
small and are worth revisiting as the surrounding phases touch them. Filing them
so they are not rediscovered from scratch each time.
