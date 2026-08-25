---
title: Sum Representation Plan (SR)
category: Planning
description: Lowering multi-variant ADTs by value, converting Option and Result from discriminated records into real sums, and the niche-filling that only becomes possible once they are -- with the measurement that gates each step and the four-predicate lockstep that makes the ABI work expensive.
---

# Sum Representation (SR)

**Status:** proposal. **SR0 has run -- both halves -- and it argues against
starting SR1 for performance.** Results and method:
[benchmarks/sum-census/RESULTS.md](../../benchmarks/sum-census/RESULTS.md).
SR1-SR4 are unstarted.

**What SR0 changed.** SR0(b) removes the risk the plan was most worried about:
the whole `.value`-on-a-`none` migration surface is 47 sites, and **zero** of
them rely on reading a zero from the dead arm. SR0(a) went the other way -- real
library code barely uses sums (20 `defdata` in 727 spice files, against 244
`defopaque` and 122 `defstruct`), and the recursive hot path that motivated the
allocation report has no example program exercising it at all. So the
performance case for SR1 is weak and the remaining case for the sum work is
expressiveness, which SR0(b) says is cheap to collect.

**Not on the critical path to v1.** Every phase is a representation change to
code that already compiles and runs correctly. Read section 4 for what to do
first if only one phase gets built -- and section 5 for why the obvious
ordering is wrong.

## 0. Provenance

Three findings from one thread of allocation work, in the order they were
found:

- [multi-variant-adts-always-heap-allocate](../reported/multi-variant-adts-always-heap-allocate.md)
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
[decision record](../reported/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25).

The measured ratios from `benchmarks/adt-alloc/ceiling.c`, for calibration:
by-value alone **1.8x**, reclamation alone **2.6x**, both **18x**, slab
**2.1x** (measured in-compiler). Note those were all measured on `logic.tur`'s
shapes, which are entirely recursive -- so they price SR4, **not** SR1. There
is currently no measurement of SR1's win, which is what SR0(a) is for.

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

### SR1 -- by-value lowering for non-recursive sums

Generalise the flat-product path to `n_ctors > 1` where the type is not
self-recursive: emit `struct { tag; union { ... } as; }` by value, sized to the
widest variant, returned in registers.

Covers 66 of 87 types. Removes the malloc **and** the leak for all of them.

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
pointer), so **1.8x is this phase's number, not SR1's**.

**Gate:** the existing report's ordering stands -- reclamation first, because
1.8x alone is not worth an ABI change of this size and 18x needs both.

**The reclamation half is no longer blocked.** It was described here as blocked
on the `rc/of` coupling that parked the slab allocator; that framing assumed
reclamation meant the slab. It does not. The slab is
[shelved](../reported/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25)
-- it never addressed the footprint half of the problem, and row B (real
reclamation) measures 2.6x against its 2.4x while staying flat as the heap
grows. So reclamation here means drop glue or an arena over the boxed spine,
and it is the **unblocked** half of SR4: no correctness blocker sits in front
of it, and it is worth more than the by-value half it is gated ahead of.

## 4. If only one phase gets built

SR0 has run, so this section is now a recommendation rather than a plan.

**Build none of SR1-SR4 for performance on current evidence.** SR0(a) found
that real code barely constructs sums, and the one workload that made the
allocation report look urgent (`logic.tur`) is exercised by nothing but a
synthetic benchmark.

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
