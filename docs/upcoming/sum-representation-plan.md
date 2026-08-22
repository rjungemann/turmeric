---
title: Sum Representation Plan (SR)
category: Planning
description: Lowering multi-variant ADTs by value, converting Option and Result from discriminated records into real sums, and the niche-filling that only becomes possible once they are -- with the measurement that gates each step and the four-predicate lockstep that makes the ABI work expensive.
---

# Sum Representation (SR)

**Status:** proposal. Nothing here has been built. The population scoping (SR0's
first half) is done and is what motivated the plan; every phase below is
unstarted.

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

That split is the plan's spine, because the two halves are different changes.
A non-recursive sum has a fixed size and generalises the existing flat-product
machinery. `(TPair :Term :Term)` has no finite inline size and needs
*field-level* boxing -- the value travels by value, only the self-referential
field stays a pointer.

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
and SR4 is where the reclamation question (arena / drop glue / the parked slab
allocator) actually has to be answered.

The measured ratios from `benchmarks/adt-alloc/ceiling.c`, for calibration:
by-value alone **1.8x**, reclamation alone **2.6x**, both **18x**, slab
**2.1x** (measured in-compiler). Note those were all measured on `logic.tur`'s
shapes, which are entirely recursive -- so they price SR4, **not** SR1. There
is currently no measurement of SR1's win, which is what SR0(a) is for.

## 3. Phases

### SR0 -- measure first

The SX plan's SR0 analogue gated two of its most expensive phases shut on
evidence. Do the same here. Half of this is already done.

**SR0(a) -- the construction census. NOT STARTED.**
Nothing yet measures how often `Option`/`Result`/small sums are actually
constructed in a real workload, and 8 bytes only matter at volume. Instrument
the emitted ctors (or count statically at emit time and dynamically with a
counter build) across the fixture corpus and the spices checkout. **Gate:** if
small-sum construction is rare outside the recursive types SR4 covers, SR1 and
SR2 are cosmetic and should be shelved.

**SR0(b) -- the migration surface. PARTLY DONE.**
Counted, unambiguous:

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

**Prototype gate, and do this before writing any of it.** Take one 2-variant
non-recursive sum (`ArithError` in `stdlib/rational.tur` is the cleanest -- two
nullary variants, no payload) and force it by value behind a compile-time seam.
The question the prototype answers is whether the four-predicate lockstep
generalises to a type carrying a tag word, or whether the tag breaks crossings
that a flat product never exercised. If the fixture damage is on the order of
the nested-monomorph fix (8 fixtures, 4 predicates), SR1 is a week. If the tag
word turns out to be load-bearing at the match and field-read seams, it is not.

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
1.8x alone is not worth an ABI change of this size and 18x needs both. The
reclamation half is still blocked on the `rc/of` coupling that parked the slab
allocator.

## 4. If only one phase gets built

**SR0(a).** It is cheap, and it is the only thing that can tell you whether any
of the rest is worth doing. The plan's whole case rests on small-sum
construction being common enough that 8 bytes and a malloc matter, and that
has not been measured.

If two: **SR0(a) then SR1.** SR1 is the enabler for SR2 and SR3, and it is the
only phase that closes both halves of the allocation report for a majority of
the affected types.

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
