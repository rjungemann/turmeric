---
title: Sum Representation Plan (SR)
category: Planning
description: Lowering multi-variant ADTs by value, converting Option and Result from discriminated records into real sums, and the niche-filling that only becomes possible once they are -- with the measurement that gates each step and the four-predicate lockstep that makes the ABI work expensive.
---

# Sum Representation (SR)

**Status: SR1 is BUILT and ON by default (2026-08-26).** SR2-SR4 unstarted.

**SR0's verdict -- "do not start SR1 for performance" -- was wrong, and section
5 of this plan says why.** SR0(a) and the SR1 gate both priced SR1 against
`stdlib/logic.tur`, a workload built entirely from *recursive* sums and
therefore structurally blind to the phase being judged; section 5 records that
exact trap and the recommendation fell into it anyway. Measured on a
non-recursive sum -- which is what SR1 covers -- it does not shave a constant
factor off allocation, it removes the allocation: 1005 allocations and 24,112
leaked bytes go to zero, and a 2e6-construction loop drops from 62.6 MB peak
RSS to 1.2 MB. Both halves of the allocation report close together, with no
reclamation, drop glue, arena or ownership analysis.

The lesson generalises past this plan: **a phase gate is only as good as the
workload it is measured on, and "the workload we already have a harness for" is
how you end up measuring the wrong one.**

SR0's results and method remain valid as a census:
[benchmarks/sum-census/RESULTS.md](../../benchmarks/sum-census/RESULTS.md).

**What SR0 changed.** SR0(b) removes the risk the plan was most worried about:
the whole `.value`-on-a-`none` migration surface is 47 sites, and **zero** of
them rely on reading a zero from the dead arm. SR0(a) went the other way -- real
library code barely uses sums (20 `defdata` in 727 spice files, against 244
`defopaque` and 122 `defstruct`), and the recursive hot path that motivated the
allocation report has no example program exercising it at all.

That was read as "the performance case for SR1 is weak". It does not support
that: it is a census of how much sum-constructing code exists, which SR0(a)
itself flags as weak evidence ("a language whose sums malloc and never free
trains its users toward `defopaque` and `defstruct` -- which is the
distribution observed"). It says nothing about what the change is worth *per
construction*, and per construction it removes the allocation outright. The
expressiveness case SR0(b) collects still stands on its own for SR2.

**Not on the critical path to v1.** Every phase is a representation change to
code that already compiles and runs correctly. Read section 4 for what to do
first if only one phase gets built -- and section 5 for why the obvious
ordering is wrong.

## 0. Provenance

Three findings from one thread of allocation work, in the order they were
found:

- [multi-variant-adts-always-heap-allocate](../archive/multi-variant-adts-always-heap-allocate.md)
  -- every `defdata` with more than one variant mallocs on construction however
  small, and nothing frees it. ~85% of executed instructions on an
  allocation-heavy stdlib workload are inside `malloc`.
- Its **Scope** section, added after the population was actually counted, which
  corrected the report's own headline claim: `Option` and `Result` are not sums.
- [byvalue-adt-app-rejects-nested-monomorphs](../archive/byvalue-adt-app-rejects-nested-monomorphs.md)
  -- fixed. Its
  [paper trail](../archive/history/byvalue-adt-app-rejects-nested-monomorphs.md)
  is the cost estimate for everything below: four predicates had to move in
  lockstep, and the tidier-looking fix broke seven passing fixtures.

## 1. What is actually true today

`adt_is_flat_product` (`types.h:380`) is the whole gate:

```c
static inline bool adt_is_flat_product(const AdtDef *def) {
    return def && def->n_ctors == 1 && !def->is_gadt;
}
```

One variant lowers flat and by value. Two variants box, on every construction,
forever. The by-value ABI exists, is proven, and simply stops at sums.

**`Option` and `Result` are single-variant records, not sums:**

```turmeric
(defstruct Option [A]   (is-some :bool) (value A))
(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))
```

So they are already on the fast path -- and they pay for it with a layout that
holds every arm at once:

```c
typedef struct { bool is_ok; int64_t ok_val; const char *err_val; }
        tur_adt_Result__int__cstr;                       /* 24 bytes */
```

**The population** (`stdlib/` + `tests/fixtures/`, counted): **87** multi-variant
non-parametric `defdata`, of which **21 are self-recursive** (`Term`, `Subst`,
`Stream`, `Regex`, `RxCls`, `RxPos`, `RxStrs`, plus fixture trees and lists) and
**66 are not**.

That split was written as the plan's spine, on the reasoning that a
non-recursive sum has a fixed size and generalises the existing flat-product
machinery while `(TPair :Term :Term)` has no finite inline size and needs
*field-level* boxing.

**The SR1 gate disproved that second half** ([sr1-gate-results.md](sr1-gate-results.md)).
A recursive ADT field already rides the int64 carrier, so such a type has a
fixed inline size and lowers by value today -- a self-recursive two-variant
`Tree` compiles and runs correctly under the seam. Field-level boxing is not a
prerequisite; it is what the carrier already does. The SR1/SR4 split below, and
the claim further down that row C's 1.8x prices "the harder variant", both need
revisiting.

**What shipped keeps the 66/21 split anyway, for a different reason.** SR1 as
built excludes recursive sums (`AdtDef.is_self_recursive`), not because they
cannot be laid out by value -- the gate settled that -- but because the thing
that actually blocks them is library source: `stdlib/logic.tur` ascribes
carrier-erased polymorphic results back to a sum type (`(:: (f s) :Subst)`), a
no-op cast while `Subst` rides the carrier and a hard `TUR-E0295` once it does
not. So the phase boundary survives with its rationale replaced: SR1 is the
population that needed no source changes, SR4 is the population that does.

**Sizes, measured:**

| | today | as a sum | |
|---|---:|---:|---|
| `Result<int,cstr>` | 24 B | 16 B | payloads unioned instead of side by side |
| `Option<int>` | 16 B | 16 B | no win -- one payload either way |
| `Option<ptr>` | 16 B | 8 B | **only** via niche filling (SR3) |

**And the dead-arm write.** The ok path constructs a value into the arm it is
not using:

```c
ctor_Result__int__cstr(true, x, ((const char *)0))
```

Every error type must therefore have a zero value. An affine type, or one
carrying drop glue, in the unused slot is a real constraint -- and this is
probably worth more than the 8 bytes.

## 2. What each phase is worth, and what it does not fix

**SR1 removes the leak for the types it covers, not just the malloc.** This is
the point most easily missed: the leak in the report is a *consequence* of
boxing. A by-value sum is never malloc'd, so there is nothing to free. SR1
therefore closes both halves of that report for 66 of 87 types without any
ownership analysis, drop glue, or slab allocator.

The 21 recursive types still box their spine and still leak. They need SR4,
and SR4 is where the reclamation question (arena / drop glue) actually has to
be answered. The slab allocator is no longer one of the candidates -- it was
**shelved** on 2026-08-25; see the
[decision record](../archive/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25).

The measured ratios from `benchmarks/adt-alloc/ceiling.c`, for calibration.
**Re-measured 2026-08-25** after the harness was found missing from the tree and
reconstructed ([RESULTS.md](../../benchmarks/adt-alloc/RESULTS.md)):

| | vs today |
|---|---:|
| by-value alone (C) | 1.41x |
| reclamation alone, per-node free (B) | 2.49x |
| both, per-node free (D) | 4.50x |
| **reclamation alone, arena (G) -- no ABI change** | **7.64x** |
| **both, arena (F)** | **10.95x** |
| slab (E) | 1.72x |

Note those were all measured on `logic.tur`'s shapes, which are entirely
recursive -- so they price SR4, **not** SR1.

**SR1's win has since been measured directly, and it is not on this scale at
all.** These rows all price how much *cheaper* an allocation gets. SR1 does not
make the allocation cheaper; on a non-recursive sum there is no allocation
left: 1005 allocations and 24,112 leaked bytes go to zero on the guarding
fixture, and a 2e6-construction loop drops from 62.6 MB peak RSS to 1.2 MB.
Wall-clock is not quoted against these ratios and should not be: with the
allocation gone the loop becomes foldable, so the two binaries stop measuring
the same work.

**The ordering rationale in this plan does not survive the two new rows.** Row G
is region reclamation over today's boxed layout -- none of SR1, none of SR4, no
ABI change -- and it reaches 7.64x, about 70% of the best number on the board.
The whole of the sum-representation work is the increment from 7.64x to 10.95x.
And by-value is worth proportionally *less* the cheaper allocation gets (+81%
over per-node free, +43% over an arena), so doing reclamation first actively
shrinks what SR1/SR4 are competing for.

The previously published "18x needs both" is withdrawn: 18x implied ~5 ns/op
with a malloc still in the loop, which is not reachable. What the transformative
number needs is a *region*, not the ABI change.

## 3. Phases

### SR0 -- measure first

The SX plan's SR0 analogue gated two of its most expensive phases shut on
evidence. Do the same here. Half of this is already done.

**SR0(a) -- the construction census. DONE.** Harness in
`benchmarks/sum-census/`; full results and method in its `RESULTS.md`.

Per-constructor counters injected into emitted C (not a codegen flag -- the
measurement must not perturb what it measures). 2148 fixtures attempted, 92%
built and ran; plus `examples/` and a declaration profile of the spices
checkout, which the dynamic census cannot reach because spice tests need
vendored C headers only `tur build` resolves.

**Gate verdict: do not start SR1 for performance.** Breadth across the 527
fixtures that construct anything: 76 construct a non-recursive sum (SR1), 16 a
recursive one (SR4). Across 727 spice files there are **20** `defdata` against
244 `defopaque` and 122 `defstruct`, and several of those 20 are single-variant
`Roll` carriers. `datalog` is the one real program that is sum-heavy;
`minikanren` constructs nothing at all and does not use `stdlib/logic.tur`
despite its name -- so the recursive hot path behind the allocation report has
no example program exercising it.

Two things this census cannot do, both load-bearing:

- **Depth is unusable.** The median fixture constructs 2 values and 98% of the
  246,080 total comes from two GC stress fixtures looping deliberately. Any
  share taken over that total describes those two fixtures. The first
  "Option/Result are 0.2% of constructions" reading was exactly that artifact
  and is withdrawn.
- **It measures the ecosystem under today's costs.** A language whose sums
  malloc and never free trains its users toward `defopaque` and `defstruct` --
  which is the distribution observed. Low sum usage is weak evidence that sums
  are unwanted and reasonable evidence that they are currently expensive.

**SR0(b) -- the migration surface. DONE, and it is not a blocker.**

Resolved with the type checker as the oracle rather than grep: rename `Option`'s
field, make stdlib self-consistent, and sweep -- every `no typeclass method
found for 'value'` is an Option site and nothing else is (`.value` on a `Ref`
still resolves). Same for `Result`'s two payload fields.

**39 Option `.value` sites across 33 files, 8 Result payload sites across 6, and
zero of the 47 rely on reading a zero from the dead arm.** Every one is guarded
or provably live; the four that are not locally obvious are still provably live.
The migration is mechanical. Outside `option.tur` itself (13 internal sites), no
stdlib file reads an Option field directly.

The original unambiguous accessor counts, for reference:

| accessor | stdlib | fixtures |
|---|---:|---:|
| `.is-some` | 13 | 39 |
| `.is-ok` | 7 | 18 |
| `.ok-val` | 7 | 6 |
| `.err-val` | 7 | 2 |

`.value` reports 942 occurrences tree-wide but is **not** a usable number:
`Option` and `Ref` both declare a field named `value`, so the count is an upper
bound dominated by unrelated types. Disambiguating it is the rest of SR0(b),
and it matters because of the hazard in section 5.

### SR1 -- by-value lowering for non-recursive sums -- DONE (2026-08-26)

Generalise the flat-product path to `n_ctors > 1` where the type is not
self-recursive: emit `struct { tag; union { ... } as; }` by value, sized to the
widest variant, returned in registers.

Covers 66 of 87 types. Removes the malloc **and** the leak for all of them.

**Shipped on by default.** `g_sr1_sum_byvalue` defaults true;
`TUR_SR1_SUM_BYVALUE=0` restores the carrier for bisecting a suspected
representation bug. `adt_is_flat_product` still reports false for these, so the
tagged-union typedef, the tag store and the tag test in `match` are unchanged --
only the ABI moved, which is what separating "flows by value" from "has no tag"
buys. Guarded by `tests/fixtures/sr1-sum-byvalue` (a `requires.leak-check`
fixture with real teeth: 24,112 bytes leaked under the seam-off carrier, zero
with it on) plus its `expected.c` snapshot.

The gate's failure list was the worklist and it held up: eight codegen
crossings, one latent bug (`adt_field_c_type` handing every caller an interior
pointer into one static buffer, which mistyped a `Result` monomorph's ok arm
once both arms could be by-value ADTs), and one scope decision. Two findings
worth carrying into SR4:

- **Every crossing had to be narrowed to by-value SUMS, not by-value ADTs.**
  Keyed on the broader predicate the new bridges also fired on single-variant
  products, which have ridden the by-value ABI since B3 and already have a more
  specific rule for each crossing -- often one that knows whether the value
  escapes into a heap container. The blunter bridge on top double-boxed them,
  breaking 27 vec/map/inline-C fixtures containing no sum at all.
- **A cycle in the inline-by-value field graph is a silent miscompile.**
  `adt_is_byvalue_product_d` walks that graph under a depth budget, so on a
  cycle it answers the same question differently depending on where the walk
  entered -- and the typedef emitter enters at a different point than a
  field-store site. `adt_graph_reaches` now declines such a type outright.

Full record: the **Resolution** section of
[multi-variant-adts-always-heap-allocate](../archive/multi-variant-adts-always-heap-allocate.md).

**Prototype gate: RUN.** Full results in
[sr1-gate-results.md](sr1-gate-results.md). The seam is in the tree as
`TUR_SR1_SUM_BYVALUE=1`, default off and provably inert (zero snapshot drift,
suite at its 2696-green baseline).

Three crossings fixed, **33 fixtures still red**, clustering into five error
shapes dominated by one -- the byval-to-carrier bridge family, the same
machinery the nested-monomorph fix had to teach about a new case. Only 14 sites
consult `adt_is_flat_product` and most are legitimate tag/layout decisions, so
this is not a long tail. Codegen half: about five predicate families, a week.

One of the three was a **silent miscompile**, not a build error: the by-value
ctor emitter never stored a tag (its comment said "byval implies single-variant
flat product"), so a nullary variant returned an uninitialised struct and the
probe printed `red`/`3` for `green`/`12`. This family of change fails quietly.

**The gate also found the blocker that is not codegen at all.**
`stdlib/logic.tur` ascribes carrier-erased polymorphic results back to sum
types -- `(:: (f v) :Stream)` -- which is a no-op cast while `Stream` rides the
carrier and a hard `TUR-E0295` once it does not. Library source depends on the
carrier representation, and that must be rewritten or specialized around. That
is the part to scope before committing, and it lands on the one module the
allocation numbers came from.

**Gating pattern:** follow `g_adt_app_byvalue` (`types.c:3182`) -- a
compile-time seam, not an `EXPERIMENTS[]` row, since this is a representation
change with no user-visible surface. SR2 is different; see below.

### SR2 -- Option and Result as real sums

The payoff phase, and the reason SR1 is worth doing.

```turmeric
(defdata Option [A]   (None) (Some A))
(defdata Result [A B] (Ok A) (Err B))
```

Buys: `Result` 24 -> 16 bytes, the dead-arm write gone (so `E` no longer needs
a zero value), and SR3 becomes possible at all.

**Hard prerequisite: SR1.** Doing SR2 first would take the two most-used types
in the language from by-value to heap-allocated-and-leaked. That is a severe
regression, not an improvement.

**This one is user-visible**, so per the experimental-features rule in
`CLAUDE.md` it wants an `EXPERIMENTS[]` row with every descriptor field
populated, `experiment_warn_if_used` at the elaboration entry point, and
`plan_path` pointing here.

### SR3 -- niche filling

Once `Option` is a real sum, `None` can be represented as the null pointer for
pointer-payload elements, taking `Option<ptr<T>>`, `option<Vec T>`,
`option<Cons T>` from 16 bytes to 8.

Plausibly the largest of the three size wins, given how common those shapes are
-- `option<vec<...>>` and `option<Cons ...>` were exactly the shapes the
nested-monomorph fix touched. **Gate:** needs SR2, and needs SR0(a) to show the
volume is there.

### SR4 -- field-level boxing for recursive sums

The remaining 21 types, `Term` / `Subst` / `Stream` among them. The value
travels by value; only the self-referential field stays a pointer. This is what
`benchmarks/adt-alloc/ceiling.c`'s row C models (`VSubst` keeps `next` as a
pointer), so **1.41x is this phase's number, not SR1's** (re-measured
2026-08-25; it was published as 1.8x).

**Gate:** reclamation first -- and on the re-measured numbers that is no longer
a sequencing preference but the substance of the whole thing.

**The reclamation half is no longer blocked**, and it is worth far more than
this phase. It was described here as blocked on the `rc/of` coupling that parked
the slab allocator; that framing assumed reclamation meant the slab. It does
not. The slab is
[shelved](../archive/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25)
-- it never addressed the footprint half of the problem, and it re-measures at
1.72x against real reclamation's 2.49x (per-node) or 7.64x (arena), while being
the only proposed fix that still climbs with heap size.

So reclamation here means drop glue or an arena over the boxed spine, no
correctness blocker sits in front of it, and **an arena over today's layout
reaches 7.64x with none of SR1 or SR4 done at all** -- roughly 70% of the best
number measured. SR4 on top of it is the increment from 7.64x to 10.95x.

**Which makes the honest recommendation for this phase: do the arena first and
then re-ask whether SR4 is worth starting.** By-value is worth +81% when
allocation is expensive and +43% when it is cheap, so the reclamation work
shrinks SR4's own payoff. SR4 should be justified against the post-arena
baseline, not against today's.

## 4. If only one phase gets built

SR0 has run, so this section is now a recommendation rather than a plan.

~~**Build none of SR1-SR4 for performance on current evidence.**~~
**Withdrawn 2026-08-26.** SR1 was built and shipped on by default; see the
status header. The reasoning below is preserved because the way it went wrong
is worth keeping: SR0(a) found that real code barely constructs sums, and the
one workload that made the allocation report look urgent (`logic.tur`) is
exercised by nothing but a synthetic benchmark -- both true, and neither a
measurement of the phase being judged. `logic.tur` is built entirely from
recursive sums, which SR1 does not touch.

The advice still holds for **SR4**, whose population `logic.tur` *is*, and
whose gate (reclamation first) is unchanged.

If the sum work is taken up, take it up **for expressiveness** -- the dead-arm
default that forces every `E` in a `(Result A E)` to have a zero value, which
rules out affine types and types carrying drop glue in the unused slot. That
argument does not depend on any of the volume numbers, and SR0(b) shows the
migration it implies is 47 mechanical sites.

In that case the order is unchanged and still mandatory: **SR1 then SR2**, never
the reverse (see section 5).

## 5. The trap in the obvious ordering

`Option` and `Result` are the most-used types in the language, so the instinct
is to convert them first and let the rest follow. That ordering is backwards
twice over:

1. **SR2 before SR1 is a regression**, per above -- from by-value to
   heap-allocated-and-leaked, on the hottest types there are.
2. **`(.value o)` on a `none` today silently reads a zero.** As a sum it becomes
   a partial operation. Any code leaning on the current lenient read has to
   change, and SR0(b) has not yet established how much code that is, because
   the `.value` count is contaminated by unrelated types that declare the same
   field name.

There is a third, subtler one worth recording. An earlier revision of the
allocation report priced by-value lowering against `logic.tur` and concluded
the non-recursive majority "buys nothing." That is true of that benchmark and
false of the language: `logic.tur` is built entirely from recursive types, so
it is structurally blind to SR1. Measuring the easy change against a workload
that cannot see it is how SR1 got under-sold in the first place, and it is why
SR0(a) is specified over the corpus rather than over one benchmark.
