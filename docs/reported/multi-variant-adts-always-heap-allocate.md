# Multi-variant ADTs always heap-allocate, and are never freed

**Severity:** medium. Not a correctness bug -- it is a constant factor on every
`defdata` with more than one variant. Measured at **~85% of executed
instructions** on a representative stdlib workload.

(An earlier revision said "every sum type in the tree, including `Option` and
`Result`". `Option` and `Result` are not sums -- see **Scope** below.)

**Status:** OPEN. Diagnosed and priced, not fixed. The fix is planned in
[docs/upcoming/sum-representation-plan.md](../upcoming/sum-representation-plan.md)
(SR), which also records the interaction this report understates: by-value
lowering removes the LEAK as well as the malloc for the types it covers, since
a value that is never boxed has nothing to free.

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
   [fixed](../archive/rc-of-adt-leaks-the-payload.md), which means `rc/of` now
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
([report](minikanren-example-implements-no-minikanren.md)).

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
