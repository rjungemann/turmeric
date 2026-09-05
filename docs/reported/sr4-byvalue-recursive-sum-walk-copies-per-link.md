# A by-value recursive sum copies 120 bytes per link when walked, and 2/3 of it is redundant

**Severity:** medium. Not a wrong answer -- a growth-rate regression on
traversal of any self-recursive `defdata`. Measured at **6.8x** on
`stdlib/logic.tur`'s `Subst` chain at n=512 bindings, and the ratio is still
climbing at the top of the sweep.

**Status:** OPEN. Found 2026-09-03 while re-running the SX2 gate benchmark
against v0.43.0.

**Fix (1) landed 2026-09-04, and it reframes this report -- see "What (1)
actually bought" below.** The copies are real and one of the three is gone, but
they are NOT the dominant term, so the plan this report laid out ("remove the
copies and the cost goes with most of them") is not supported by its own first
data point. Read that section before spending time on (2) or (3).

## What happens

SR4's default flipped to by value on 2026-09-02 (RM4). A self-recursive sum
now travels as a `tag + union` aggregate with the recursive field boxed. For
`stdlib/logic.tur`:

```c
sizeof(tur_adt_Term)  == 24
sizeof(tur_adt_Subst) == 48
```

`subst-lookup` walks the `SBind` chain. Its emitted recursive arm:

```c
case 1: {
    int64_t v_1597 = (int64_t)__scrut->as.SBind._0;
    tur_adt_Term term_1598 = __scrut->as.SBind._1;                                 /* (a) */
    tur_adt_Subst rest_1599 = *(tur_adt_Subst *)(intptr_t)(__scrut->as.SBind._2);  /* (b) */
    tur_adt_Lookup __t308;
    if ((v_1597) == (vid)) {
        __t308 = (ctor_Lookup_LFound(term_1598));
    } else {
        tur_adt_Subst __t310 = rest_1599;                                          /* (c) */
        __t308 = (subst_hylookup(vid, &__t310));
    }
    ...
```

Three copies per link, 120 bytes total, and two of the three are avoidable:

- **(a)** binds the 24-byte `Term` field before the branch that decides whether
  it is used. On the recursive (miss) path it is dead. Every match arm
  materializes every binder at arm entry regardless of which sub-branch uses it.
- **(b)** dereferences the boxed spine pointer and copies the whole 48-byte
  node out of the box, when the callee's parameter is a `const tur_adt_Subst *`
  and could take `(const tur_adt_Subst *)(intptr_t)__scrut->as.SBind._2`
  directly.
- **(c)** copies that copy a second time into `__t310`, purely so the call has
  an addressable lvalue -- but `rest_1599` already is one.

The carrier path moves a single `int64_t` per link and does none of this.

## Measurement

`benchmarks/bench-logic-subst.tur` (Release, one box, one binary per arm; full
table and method in
[benchmarks/logic-subst-results.md](../../benchmarks/logic-subst-results.md)).
The workload is bind n, walk all n, discard.

| bindings | default (by value) | `TUR_SR4_RECURSIVE_CARRIER=1` | ratio |
|---:|---:|---:|---:|
| 1 | 112.8 ns/op | 109.7 | 1.03x |
| 8 | 132.6 | 95.2 | 1.39x |
| 64 | 419.2 | 143.4 | 2.92x |
| 512 | 3297.7 | 486.7 | **6.78x** |

Adding `TUR_SR1_SUM_BYVALUE=0` on top changes nothing -- `logic.tur`'s hot
types are all self-recursive, so SR1 is invisible here and SR4 is the whole
effect. That is the control.

**The crossover is around n=4-8.** Below it the by-value form is at parity or
slightly ahead (fewer mallocs per construction); above it the per-link copies
dominate and the gap grows with chain length.

## How this sits against RM4's decision

RM4 (2026-09-02) flipped the default on "~1.03x time for 1.8x less memory on
`logic.tur` (370 -> 202 MB peak RSS at 400k passes)", recorded in the SR plan's
SR4 section as `logic.tur bind+walk, 400k passes, n=8`.

**That ratio does not reproduce here: at n=8 this benchmark measures 1.39x.**
Stated plainly rather than explained away -- RM4's harness is not committed
alongside its numbers, so the difference cannot be attributed from the record,
and it may be a different program, a different box, or a different unit. What
this report can defend is its own A/B, which is one box, one compiler, one
binary per arm, three runs, medians.

**The load-bearing point does not depend on resolving that.** The ratio is not
a constant -- it grows monotonically with chain length, from parity at n=1 to
6.8x at n=512 -- so a measurement taken at ONE n cannot see it whichever n that
was, and the pass count is the wrong axis to sweep because it holds chain
length fixed. Construction really did get cheaper and the memory number is
real; the trade simply has a term in it that the reported measurement had no
way to price.

The SR plan's own risk list already names this trap for `logic.tur` in both
directions ("once under-selling SR1 because `logic.tur` is structurally blind
to it, once over-selling the ceiling because `logic.tur` is all of the
population it prices"). This is a third variant: right workload, wrong axis
swept.

**This is not a request to unflip.** The three copies below are removable
without touching the default, and if they go the cost this report is about goes
with most of them.

## What to do

In descending order of value-per-cost, and none of these need the default to
change:

1. **Do not copy the node to take its address** (c). `rest_1599` is already an
   lvalue; `&rest_1599` is the same pointer with one fewer 48-byte copy. This
   looks like an unconditional "materialize a temp for the call argument" rule
   that does not check whether the operand is already addressable.
2. **Pass the box through without dereferencing it** (b). The callee's
   parameter is already a `const *`, so a recursive-field binder consumed only
   as a by-reference callee argument never needs the node copied out at all.
   Together (1) and (2) are the 96 of the 120 bytes.
3. **Sink match binders to the arms that use them** (a). Broader than this
   report -- it is a general match-lowering improvement -- and worth its own
   assessment, since a binder can be read by a guard.

Only if all three land and the ratio is still material does the SR4 default
itself deserve a re-look. Re-run `benchmarks/bench-logic-subst.tur`, which
reports the A/B directly, rather than a pass-count sweep.

## Scope -- who else pays this

Any `defdata` that is self-recursive AND walked, which is the 21 recursive sums
the SR plan counted. In stdlib that is `logic.tur` (`Term`, `Subst`, `Stream`)
and `re.tur` (`Regex`, `RxCls`, `RxPos`, `RxStrs`). RM0's corpus census found
the recursive-spine allocation concentrated in two fixtures (a lazy-stream one
and a regex engine at 97% of 4,205 allocations), so those two are where a
traversal cost would show up next.

`re.tur` was measured by RM4 as "no slower at 1.2x less" -- worth re-checking
on the same axis, since a regex match walks its pattern tree the way
`subst-lookup` walks its chain.


## What (1) actually bought (measured 2026-09-04)

Copy (c) is gone: the pass-by-pointer spill no longer materializes a temp when
the operand is already an addressable variable, so `subst-lookup`'s recursive
arm emits `subst_hylookup(vid, &rest_1589)` instead of
`tur_adt_Subst __t310 = rest_1589; subst_hylookup(vid, &__t310)`.  That is 48 of
the 120 bytes per link -- 40% of the copy volume this report is about.

A/B on one box, one compiler, one binary per arm, medians of three
(`benchmarks/bench-logic-subst.tur`, the persistent arm):

| bindings | before (1) | after (1) | change |
|---:|---:|---:|---:|
| 8 | 135.2 ns/op | 126.2 | -6.7% |
| 32 | 294.5 | 260.1 | -11.7% |
| 128 | 1043.7 | 1036.6 | -0.7% |
| 512 | 4759.7 | 4512.5 | -5.2% |

**Roughly 5%, and inside this box's noise at several points** (n=1..4 measured
*slower*, which a pure copy removal cannot cause -- that is the noise floor
showing).  Call it 5%, not 40%.

And against the carrier arm, after the fix:

| bindings | by value + fix (1) | `TUR_SR4_RECURSIVE_CARRIER=1` | ratio |
|---:|---:|---:|---:|
| 64 | 518.1 | 131.8 | 3.93x |
| 128 | 1053.0 | 188.8 | 5.58x |
| 512 | 4645.1 | 526.0 | **8.83x** |

**The gap did not close; on this box it reads wider than the 6.78x this report
measured on another.**  Both numbers are A/Bs against their own box, so the
comparison across them is not meaningful -- what IS meaningful is that removing
40% of the copy volume moved the ratio by about 5%.

### What that means for (2) and (3)

The load-bearing claim of this report was that the three copies are the cost.
On the evidence of the first one, they are not.  (2) is the other 48 bytes and
would plausibly buy another ~5%; (3) is 24 bytes and broader in blast radius.
Neither is going to turn 8.8x into parity, so neither should be attempted on
the strength of this report's reasoning alone.

The next step is a PROFILE, not another copy removal.  Candidates the copy
model does not account for, in the order they are worth checking:

- **Cache footprint.** The by-value spine is a 48-byte node per link plus a box
  pointer; the carrier spine is an 8-byte word per link.  A walk touches every
  link, so the miss rate scales with node size, and the ratio growing
  monotonically with chain length is what a cache effect looks like.
- **Construction, not traversal.** The workload is bind n, walk n, discard, and
  the by-value path mallocs a box per link at bind time.  This report attributed
  the whole gap to the walk without separating the two phases.
- **The deref itself** (b), which is a dependent load per link -- the latency
  may matter more than the 48 bytes moved.

Only the third is a copy, and it is the one (2) would remove.

Fix (1) stands on its own terms regardless: it removes work that was doing
nothing, at zero risk, and it is pinned by the two regenerated snapshots
(`conv-byval-adt-nested-inline`, `conv-multi-variant-tagged`) which now show
`area(&c_1442)` where they showed a temp and a copy.

## Does any of this leak? (checked 2026-09-04)

Asked because a report about copying a boxed spine invites the question, and
because fix (1) removed a temp.

**Fix (1) does not.** The spill it skips was a plain stack local with no drop
glue -- the `free()` beside it in the emitter frees the temp's NAME string at
compile time, not memory at run time -- so removing it cannot remove a runtime
free.  Verified rather than reasoned: both regenerated fixtures leak-check
clean under ASan with a compiler built from `059b44a1~1` and with one built
from `059b44a1`, no delta.

**The spine does, and it is RM2, not a new finding.** A 64-link `Subst` chain
leaks 64 allocations, one per link, and it GROWS per discarded round -- 100
rounds of an 8-link chain leak 800 boxes, not 8:

| arm | 64-link chain | per link |
|---|---:|---:|
| by value (default) | 3072 B in 64 allocations | 48 B |
| `TUR_SR4_RECURSIVE_CARRIER=1` | 1536 B in 64 allocations | 24 B |

**The counts are equal**, so this is not something the SR4 default flip
introduced -- by value makes each leaked box bigger, not more numerous.  It is
[reclamation-plan.md](../upcoming/reclamation-plan.md)'s **RM2**, which names
this exact allocation ("the per-node spine box of a self-recursive sum, which
is what `logic.tur` allocates") and explains why RM1's scope-exit rule cannot
reach it: "a tree's nodes escape their constructor by construction". RM2 is
gated on RM0(b) and was deliberately not started -- `leak-sweep-decomposition`
records "RM2, which RM0 closed: no constituency".

So: nothing to file. The one thing this adds to RM2's record is the growth
shape, since RM0 priced the spine as an allocation count and not as unbounded
growth in a program that builds and discards chains in a loop -- which is what
a solver backtracking does. If RM2's gate is ever revisited, that is the
measurement to revisit it with.