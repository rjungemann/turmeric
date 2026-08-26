# Multi-variant ADTs always heap-allocate, and are never freed

**Severity:** medium. Not a correctness bug -- it is a constant factor on every
`defdata` with more than one variant. Measured at **~85% of executed
instructions** on a representative stdlib workload.

(An earlier revision said "every sum type in the tree, including `Option` and
`Result`". `Option` and `Result` are not sums -- see **Scope** below.)

**Status: RESOLVED 2026-08-26 for the non-recursive sum population** -- SR1
built and turned ON by default. Recursive sums are unchanged and still ride the
carrier; they are SR4's population and what still blocks them is written up
below. See **Resolution** immediately below for what shipped, what it
measured, and what is left.

The fix is planned in
[docs/upcoming/sum-representation-plan.md](../upcoming/sum-representation-plan.md)
(SR), which also records the interaction this report understates: by-value
lowering removes the LEAK as well as the malloc for the types it covers, since
a value that is never boxed has nothing to free. That interaction is the one
the resolution turned on -- both halves of this report closed together, with no
ownership analysis, drop glue, or allocator work at all.

## Resolution (2026-08-26) -- SR1, non-recursive sums, on by default

`g_sr1_sum_byvalue` now defaults ON. A **non-recursive**, non-parametric,
non-`:heap`, non-GADT multi-variant sum flows by value as a `tag + union`
aggregate. The tagged-union layout is unchanged -- the tag word, the tag store
in each constructor and the tag test in `match` are all exactly as they were.
Only the ABI moved. `TUR_SR1_SUM_BYVALUE=0` restores the int64 carrier for
bisecting a suspected representation bug; it is an escape hatch, not a mode.

**Both causes close together, and neither needed the work this report expected.**
Cause 1 (always boxed) goes away because the constructor returns the aggregate.
Cause 2 (never freed) goes away as a *consequence*: a value that is never
malloc'd has nothing to leak. No reclamation, no drop glue, no arena, no
ownership analysis.

**Measured.** `tests/fixtures/sr1-sum-byvalue` constructs a two-variant `:copy`
sum 1000 times in a loop:

| | allocations | leaked |
|---|---:|---:|
| `TUR_SR1_SUM_BYVALUE=0` (this report's subject) | 1005 | 24,112 B |
| default (SR1 on) | 0 | 0 |

A 2e6-construction loop over the same shape: **peak RSS 62.6 MB -> 1.2 MB**.
Wall-clock is not quoted, and deliberately: with the allocation gone the loop
becomes foldable, so the two binaries are not measuring the same amount of
work. Memory is the honest number, and it is the half this report's own
correction says matters ("a memory-footprint problem, not (at these scales) a
time problem").

**What is NOT fixed: recursive sums.** `Term`, `Subst`, `Stream`, `Regex` and
the other 21 self-recursive sums still box on every construction and still
leak. That is the workload this report measured, so **the callgrind numbers at
the top of this report are unimproved.** They are SR4's population.

The exclusion is not a layout limit. A recursive ADT field already rides the
int64 carrier, so such a type has a finite inline size and lowers by value
perfectly well -- the SR1 gate proved that, and every codegen crossing SR1
fixed serves it too. What holds SR4 back is *library source*:
`stdlib/logic.tur` ascribes carrier-erased polymorphic results back to a sum
type (`(:: (f s) :Subst)`), a no-op cast while `Subst` rides the carrier and a
hard `TUR-E0295` once it does not. That is a rewrite of the module the
allocation numbers came from, not a predicate to widen, so SR4 should do it
deliberately. `AdtDef.is_self_recursive` is the boundary, recorded at
declaration time because a recursive field's `full_type` is deliberately NULL
and nothing downstream can otherwise tell `(SBind :int :Term :Subst)` from
three ints.

**The slab decision stands and is now moot for this population.** Row E was
shelved (below) because it addressed speed rather than footprint and needed a
whole-program escape pass. SR1 addresses footprint directly, for free, on the
types it covers.

**What the ordering advice above got wrong.** This report and the SR plan both
concluded "do not start SR1 for performance" -- SR0(a) found real code barely
constructs sums, and SR1 was priced at 1.41x against `logic.tur`, a workload
built entirely from *recursive* types and therefore structurally blind to it.
That reasoning was sound about `logic.tur` and wrong about the change: measured
on a non-recursive sum, which is what SR1 actually covers, it is not a constant
factor on allocation cost -- it removes the allocation. The plan's own section 5
flags this exact trap ("measuring the easy change against a workload that
cannot see it") and the recommendation still fell into it.

**Cost, for calibration against the gate's estimate.** The gate predicted "five
predicate families and a week" for the codegen half. The real shape was eight
crossings, one latent bug, and one scope decision:

- `match`'s switch path could not bind an inline by-value aggregate field.
- The constructor-argument emitter read `ctors[0]` for the field being stored,
  which is the wrong arm once a by-value owner can be multi-variant.
- `match`'s if-chain tag test hardcoded `->`; a by-value sum is not a pointer.
- Typedef ordering: an inline by-value field is a real forward dependency,
  which source order never had to satisfy while every ADT field was an int64.
- Four carrier crossings for the `:int`-as-type-eraser shape `stdlib/fix.tur`
  is built on (box into an `:int` param, deref off an `:int` return, deref a
  carrier arg into a refined aggregate param, box a by-value head into a
  variadic cons cell).
- The CPS backend had its own carrier-only match lowering.
- The if-merge bridge consulted a side table that records locals, so a by-value
  aggregate *parameter* was bridged as though it were a carrier.
- `adt_field_c_type` handed every caller an interior pointer into ONE static
  buffer, so a constructor with two pointer-boxed fields mistyped all but the
  last: `ctor_Result__Rational__ArithError(bool, ArithError*, ArithError*)`.
  Latent until a `Result` monomorph could have by-value ADTs in both arms.

Two lessons worth keeping. **Every one of these bridges had to be narrowed to
by-value SUMS, not by-value ADTs.** Keyed on the broader predicate they also
fired on single-variant products -- which have ridden the by-value ABI since B3
and already have a more specific rule for each crossing, often one that knows
whether the value escapes into a heap container -- and the blunter bridge
layered on top double-boxed them, breaking 27 vec/map/inline-C fixtures with no
sum in them at all. And **a cycle in the inline-by-value field graph is a
silent miscompile, not a build error**: `adt_is_byvalue_product_d` walks that
graph under a depth budget, so on a cycle it answers the same question
differently depending on where the walk entered, and the typedef emitter enters
at a different point than a field-store site. `adt_graph_reaches` declines such
a type outright.

**Fallout in example code, and what it says.** Two programs broke, both for the
same reason: they erase a sum into an `:int` slot. `examples/datalog/*` stores
a `Value` in a raw `int64_t[4]` built in inline-C, and declared its constructor
wrappers `: int` while returning `Value`. They now declare those ADTs `:heap`
-- a typed pointer, which is one word (so the storage still works) and is what
the example actually means. This is the `:int` stand-in CLAUDE.md rules out,
and a representation change is exactly the event that collects the bill for it.

**Guarded by** `tests/fixtures/sr1-sum-byvalue` (a `requires.leak-check`
fixture: it fails with 24,112 bytes leaked under `TUR_SR1_SUM_BYVALUE=0`), plus
the `expected.c` snapshot pinning the by-value constructor shape.

**Suites at resolution:** `run.sh` 2707 passed / 0 failed; `run-turi.sh`
1861/0; `run-leak-check.sh` 54/0; `check-examples.sh` 26/0; flags, fmt, cli,
build-project, build-shared, hamt, repr-decision-ratchet, cc-warn-ratchet and
stdlib-checks all green.

## What was measured

`stdlib/logic.tur`'s substitution -- one `SBind` + one `TInt` construction and a
`logic-walk` per operation. Under `callgrind`:

```
14,579,535 (100.0%)  PROGRAM TOTALS
 9,176,082 ( 62.9%)  malloc.c:_int_malloc
 2,970,050 ( 20.4%)  malloc.c:malloc
   776,000 (  5.3%)  subst_hylookup
   368,000 (  2.5%)  logic_hywalk
```

The actual logic is under 8% of the program. Everything else is the allocator.
Wall clock agrees: **~183 ns per bind+walk**, and flat -- 179.7 / 189.9 / 183.7 /
183.6 / 183.5 ns/op at 250 / 1k / 4k / 16k / 64k passes, each in its own process.

## Cause 1 -- a multi-variant ADT is always boxed, however small

Single-variant `:copy` ADTs already lower **by value**:

```c
static tur_adt_One ctor_Only(int64_t _0) {     /* (defdata One :copy (Only :int)) */
    tur_adt_One __r;
    __r.as.Only._0 = _0;
    return __r;                                 /* no allocation */
}
```

Add a second variant and the same type heap-allocates:

```c
static int64_t ctor_A(int64_t _0) {   /* (defdata Many :copy (A :int) (B :int) (C)) */
    tur_adt_Many *__r = (tur_adt_Many *)malloc(sizeof(tur_adt_Many));
    __r->tag = 0;
    __r->as.A._0 = _0;
    return (int64_t)(intptr_t)__r;
}
```

`tur_adt_Many` is 16 bytes -- a tag and one `int64_t`. `Term` (4 variants) is 24.
Both are trivially register- or stack-passable, and both malloc on every
construction. The gate is `adt_is_byvalue_product_d` (`types.c:2918`), whose
first test is `adt_is_flat_product` -- single variant. The by-value ABI exists
and is proven; it simply stops at sums.

`:copy` is not the relevant knob: it controls linearity (may this value be used
twice), not representation. `Term` is already `:copy` and still boxes.

## Cause 2 -- nothing ever frees them

No `free` reachable from the hot path releases a `Term` or a `Subst`.
`needs_drop_glue` (`elab_structs.c:1349`) is set only for `TY_RC` / `TY_REF` /
`TY_WEAK` / boxed-fn fields -- it frees a box's *contents*, never the box -- and
`elab_effects.c:30` gates it to `n_ctors == 1` regardless. `:heap` on a sum
yields a typed pointer instead of an `int64_t` carrier but still mallocs and
still never frees. So every multi-variant ADT construction leaks, `:copy` or
not.

**This is a memory-footprint problem, not (at these scales) a time problem** --
see the withdrawn claim below.

## What each fix is worth

Five representations of the same workload (n=8 bindings, build / walk all /
discard), each timed in **its own process**, best of three:

| representation | 1k passes | 16k | 64k | vs today |
|---|---:|---:|---:|---:|
| A -- boxed, leaked (**today**) | 92.4 | 98.3 | 95.4 | 1.0x |
| B -- boxed, freed | 34.6 | 36.9 | 36.7 | 2.6x |
| C -- Term by value, spine boxed, leaked | 49.0 | 56.9 | 52.3 | 1.8x |
| D -- by value **and** reclaimed | 5.1 | 5.3 | 5.2 | **18x** |
| E -- boxed, leaked, slab-bump-allocated | 33.5 | 40.7 | 40.6 | 2.4x |

Absolute numbers are from a simplified C model of the same shapes, so they run
faster than the real compiler's 183 ns/op; the **ratios** are what transfer.

> **Re-measured 2026-08-25, and two of these rows do not hold.** The harness
> that produced the table above was never committed -- `.gitignore`'s blanket
> `*.c` swallowed `benchmarks/adt-alloc/ceiling.c`, so the commit that published
> these numbers landed with only its README and nobody could re-run them. It has
> been reconstructed (and the `.gitignore` gap fixed). Full results in
> [benchmarks/adt-alloc/RESULTS.md](../../benchmarks/adt-alloc/RESULTS.md).
>
> **B reproduces** at 2.49x, so the slab decision below stands -- and the slab
> looks *worse* on re-measurement (E is 1.72x, not 2.4x), so it stands more
> firmly. **D does not reproduce**: 4.50x, not 18x. And two new rows change what
> the whole plan is for -- see the section immediately below.

## The reclamation mechanism dominates, and the ABI change is secondary

Two representations the original table did not have, measured 2026-08-25:

| representation | vs today |
|---|---:|
| **G -- boxed, arena-reclaimed** (no ABI change at all) | **7.64x** |
| **F -- by value AND arena-reclaimed** | **10.95x** |

Row G is region reclamation over **today's boxed layout**. No by-value lowering,
no `adt_is_flat_product` work, none of SR1 or SR4 -- and it reaches 7.64x, about
**70% of the best number on the board**. Everything the sum-representation plan
proposes is the increment from 7.64x to 10.95x.

The same relationship shows up across the malloc rows: by-value is worth +81%
when allocation is expensive (B 2.49x -> D 4.50x) and +43% when it is cheap
(G 7.64x -> F 10.95x). By-value removes one of the two allocations per binding,
so **the cheaper allocation gets, the less it is worth**. Fixing the allocator
shrinks the prize the ABI change is competing for.

This also explains the 18x that would not reproduce. 18x implies ~5 ns/op while
still doing one `malloc` per binding, which is not reachable -- a malloc/free
pair costs more than that on its own. The number the original was reaching for
is row F, which reclaims by resetting a region rather than freeing per node. So
**"18x needs both" is not supported**: what the transformative number needs is a
region, and the ABI change is a multiplier on top of it.

**The caveat that keeps this honest.** An arena needs a region whose end is
known, and that -- not the bump pointer -- is the hard part; inferring one in
general is region inference. So F and G are the ceiling *for workloads with a
natural region*. The workload this report is about is exactly such a workload:
the solver's `logic.tur` substitution is built per query and discarded whole, so
a per-query arena is one reset at a call site that already exists, not an
analysis. Generalising to arbitrary user `defdata` is a separate and much larger
question that nothing here answers.

**One arena idea has been measured and rejected**, so it does not get proposed
again: a *compiler-internal* per-entry arena, resetting once per discharged
refinement obligation. It would cap a constant rather than change a growth
curve -- the only part of the solver's allocation that grows with obligation
count is VC construction, which the compiler retains on purpose, while the
resettable theory state is flat at ~267 KB whether a file has 25 obligations or
400. Numbers and method in
[../archive/history/per-entry-arena-gate.md](history/per-entry-arena-gate.md).
That result is about the compiler's own arena and says nothing about row G,
which is a different mechanism in the emitted program.

Three things follow, and the first corrects this report's original advice:

1. **By-value is not the standalone win it was billed as.** It is 1.8x, not
   "highest leverage by a wide margin". It cannot remove the spine allocation:
   `Subst` is a list, so one node per binding is inherent to the persistent
   structure. By-value removes the *Term* riding inside each node -- one of the
   two allocations, not both.
2. **Row E is the cheap one, with a caveat found while building it.** A slab
   allocator for ADT boxes measures **2.1x in the real compiler** (192 -> 89
   ns/op, checksums identical), close to the 2.4x this model predicted. It needs
   no ownership analysis and no ABI change -- but it is NOT the one-line change
   billed here. It has to be keyed on `needs_drop_glue`, because a type with
   drop glue is freed by `drop_glue_*`'s trailing `free(ptr)` and slab memory
   must never reach libc `free()`.

   **It is now blocked harder than when this was written.** The `rc/of` leak it
   was coupled to has been
   [fixed](rc-of-adt-leaks-the-payload.md), which means `rc/of` now
   FREES its ADT payload -- so a slab-allocated box handed to an `rc` reaches
   `free()`. Confirmed, not theorised: ASan reports `attempting free on address
   which was not malloc()-ed`.

   A `ctor_*` cannot know whether its result ends up in an `rc`, so no local
   predicate fixes this. The slab would need a whole-program pass marking every
   ADT def used as an `rc/of` payload and excluding those. The other half of the
   objection did resolve --
   [leak checking now exists](compiled-fixtures-are-not-leak-checked.md) via
   `tests/run-leak-check.sh`, so a bad free is catchable.

   **The slab is now SHELVED** -- see the decision record below. Row E is no
   longer "the cheap one", and row B is the better target.
3. **The transformative number needs both** (18x). That is the case for doing
   the by-value ABI work -- but only *after* the allocator, knowing it buys
   1.8x on its own, and knowing (see **Scope**) that the 1.8x requires the
   field-level-boxing variant for recursive sums. The easier two thirds of the
   population -- non-recursive sums -- buys nothing on this workload.

   With the slab shelved, "the allocator" in that ordering means **row B
   (reclamation)**, not row E.

## Decision -- the slab allocator is shelved (2026-08-25)

The seam stays in the tree, default off, and **nobody should build the
whole-program escape pass that would let it be turned on.** The decision is
recorded here rather than left implicit, because "ASan aborts, so fix the
abort" is the obvious next move and it is the wrong one.

Four reasons, in descending order of weight.

**1. It does not fix the problem this report identifies.** After the withdrawn
claim was corrected, this report's own framing is that the cost is a
*memory-footprint* problem, not a time problem at these scales. A slab makes
allocation faster and frees nothing -- slabs are 256 KB chunks released never,
which is the point of the design. It addresses the half that the measurement
says is not the half that matters. Rows B and D fix footprint; row E does not.

**2. Its one differentiator is gone.** The entire case for row E over row B was
"2.4x with no ownership analysis, no drop glue, no ABI change". Since a
`ctor_*` cannot know whether its result reaches `rc/of`, turning it on now
needs a whole-program pass over which ADT defs flow into an `rc/of` payload.
That is an escape analysis. Once it is on the table, "no ownership analysis" is
false and row E is competing with row B on level ground.

**3. On level ground it loses.** From the table above:

   | representation | 1k | 16k | 64k | vs today |
   |---|---:|---:|---:|---:|
   | B -- boxed, **freed** | 34.6 | 36.9 | 36.7 | **2.6x** |
   | E -- boxed, leaked, **slab** | 33.5 | 40.7 | 40.6 | 2.4x |

   Reclamation is the faster of the two *and* is nearly flat as the heap grows
   (+6% from 1k to 64k) where the slab degrades (+21%). The slab was always the
   cheap approximation of reclamation; it is no longer cheap, and it was never
   as good.

**Re-confirmed first-hand on 2026-08-25**, so the decision does not rest on a
secondhand note. `(rc/of (PA 7))` for a two-variant `:copy` ADT, built with
`TUR_ADT_SLAB=1` and compiled `-fsanitize=address`:

```
ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed
0x... is located 16 bytes inside of 262160-byte region
```

The region size is the tell: 262160 = the slab's 262144-byte `buf` plus its
16-byte `{next, off}` header, so the pointer handed to `free()` is provably
slab memory rather than some unrelated bad free.

**4. It is a bet against the direction of the codebase.** The slab is sound
only over boxes that are never freed. Every ownership fix shrinks the
population it is legal on -- the `rc/of` fix is exactly that, and it is what
broke it. This is not a blocker that gets fixed once; it recurs on every
future reclamation improvement, and each recurrence is another silent
`free()`-of-slab-memory waiting for someone to notice.

**And there is no constituency.** SR0(a)'s census found real code barely
constructs sums at all -- 20 `defdata` across 727 spice files against 244
`defopaque` and 122 `defstruct` -- and `stdlib/logic.tur`, the workload the
2.1x was measured on, is exercised by nothing but a synthetic benchmark.
`examples/minikanren`, the one program that ought to exercise it, constructs
nothing and does not import the module
([report](../reported/minikanren-example-implements-no-minikanren.md)).

### What stays, and what to do instead

- **The seam stays.** Three call sites plus a preamble, byte-identical codegen
  when off, so the carrying cost is ~zero and it keeps the 2.1x measurement
  reproducible. It is a museum piece, not a roadmap item.
- **Do not build the escape pass.** It is the most expensive option, it
  unlocks the weaker representation, and the analysis it requires is most of
  what reclamation needs anyway -- so it pays for the hard part and spends it
  on the smaller payoff.
- **Reclamation (row B) is the unblocked half.** 2.6x, fixes footprint rather
  than only speed, and has no correctness blocker in front of it. The ordering
  in [the SR plan](../upcoming/sum-representation-plan.md) already says
  reclamation first; this decision just removes the slab as a candidate for
  what "reclamation" means.

**What would reopen this.** A real workload -- not a benchmark -- that
constructs multi-variant ADTs hot enough to care, *and* a reason the same
workload cannot take row B instead. Both halves are required; the first alone
is what the census went looking for and did not find.

## Scope -- who actually pays this, and what "by value" has to mean

Scoped before starting the ABI work, because the answer moved the estimate.

**`Option` and `Result` are not affected.** They are `defstruct`, not `defdata`:

```turmeric
(defstruct Option [A] (is-some :bool) (value A))
(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))
```

They lower to single-variant record ADTs, so they are already flat products and
their concrete monomorphs **already go by value today**:

```c
static tur_adt_Option__int ctor_Option__int(bool _0, int64_t _1) {
    tur_adt_Option__int __r;
    __r.is_some = _0;
    __r.value   = _1;
    return __r;                                 /* no allocation */
}
```

Two Option/Result forms do still malloc, and neither is this bug:

- The un-monomorphised base `ctor_Option` / `ctor_Result`. Reached only when a
  type argument genuinely cannot be inferred -- `(let [r (ok 7)] ...)` with
  nothing constraining the error type. Annotating the enclosing function moves
  the call to `ctor_Result__int__cstr`, which returns by value. Working as
  intended.
- Monomorphs whose type argument is itself a monomorph -- `option<list<int>>`,
  `result<vec<T>, cstr>`. These *are* a live bug, filed separately as
  [byvalue-adt-app-rejects-nested-monomorphs](byvalue-adt-app-rejects-nested-monomorphs.md).
  It is a much smaller job than lowering sums and touches shapes that are far
  more common.

**The population is 87 types, and a quarter of them are the hard kind.** Across
`stdlib/` and `tests/fixtures/`, 87 multi-variant non-parametric `defdata`
exist; **21 are self-recursive** (`Term`, `Subst`, `Stream`, `Regex`, `RxCls`,
`RxPos`, `RxStrs`, plus fixture trees and lists). In stdlib alone the sums are
10: `re.tur` (4), `logic.tur` (5), `rational.tur` (1) -- `ArithError` being the
only one of those that is not recursive or does not hold a recursive type.

That split matters because it is two different changes:

- **Non-recursive sums (66 of 87)** are the generalisation the report describes:
  tag + union, fixed size, returned in registers. The existing flat-product
  machinery mostly transfers.
- **Recursive sums (21 of 87)** cannot go by value as a whole -- `(TPair :Term
  :Term)` is infinitely sized inline. They need *field-level* boxing: the value
  travels by value, and only the self-referential field stays a pointer.

And the workload this report measured is entirely in the second group. `Term`,
`Subst`, and `Stream` are all recursive, so the easy two thirds of the
population buys **nothing** on the numbers in the table above. Row C's 1.8x
already assumes field-level boxing -- `benchmarks/adt-alloc/ceiling.c`'s
`VSubst` keeps `next` as a pointer, which is exactly that -- so the 1.8x is the
price of the *harder* change, not the easier one. (The model does simplify
`Term` to its `TInt`/`TVar` leaves and omits `TPair`; field-level boxing is what
would make the real `Term` behave like the model's `VTerm`.)

**A workaround exists today with no compiler change.** A 2-variant sum with no
recursion can be re-encoded as a discriminated `defstruct` in the Option/Result
style and gets by-value lowering immediately. `Lookup` (`LMissing` / `LFound
:Term`), `UnifyResult` (`UFail` / `UOk :Subst`) and `ArithError` are all
candidates. It costs pattern-matching ergonomics, so it is worth doing only
where a profile says so -- but it means "small sums box" is not a hard blocker
for any specific hot path.

## Withdrawn: an earlier claim that this degrades 8x with heap size

An earlier revision of this report said the cost climbed from 180 to 1396 ns/op
as the heap grew. **That was a measurement artifact and is wrong.** Both the
Turmeric scaling run and the first C harness measured every pass count *in one
process, cumulatively*, so each row inherited the leaked heap of every row
before it. Re-measured with each point in its own process, the cost is flat over
a 256x range (183.5-189.9 ns/op).

The tell was there and was nearly missed: the reclaiming variants were stable
across runs while the leaking ones swung 85 to 633 ns/op for the *same*
configuration. That is not a property of leaking, it is a property of sharing a
heap with whatever ran first.

Recorded rather than quietly deleted because the flaw is easy to repeat: **a
sweep over "how much work" is not measuring scaling if the work accumulates in
one process.**

## Method note

Instruction counts come from callgrind, which counts under valgrind rather than
cycles on real hardware, so the wall-clock figures are given alongside; the two
agree on where the time goes.

An even earlier attempt measured nothing at all: a tail-recursive loop whose
result went unused folded away entirely, reporting 0 ns for 200,000 ADT
constructions -- the same trap SX0(a)'s closure baseline hit. Every measurement
here consumes and prints its result, and the five representations cross-check
each other's checksums.
