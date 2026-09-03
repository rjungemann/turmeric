---
title: Sum Representation Plan (SR)
category: Planning
description: Lowering multi-variant ADTs by value, converting Option and Result from discriminated records into real sums, and the niche-filling that only becomes possible once they are -- with the measurement that gates each step and the four-predicate lockstep that makes the ABI work expensive.
---

# Sum Representation (SR)

**Status: SR1 is BUILT and ON by default (2026-08-26).** SR2a/b built and default; SR3 slice A default, slice B behind `--enable=option-niche`; SR4 default flipped to by value 2026-09-02 (RM4).

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

**The reclamation half now has a plan of its own:**
[reclamation-plan.md](reclamation-plan.md) (RM). It carries the arena and
drop-glue work this document repeatedly defers to, and its first phase is a
re-measurement -- SR1, SR2a and SR3 slice A have removed the allocation
outright for most of the population the 7.64x was measured over, so the rows
in section 2 now price the recursive sums and the erased residue rather than
the language as a whole.

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

### SR2 -- Option and Result as real sums -- **SHIPPED (SR2b) 2026-08-27**

**Status: the conversion is done.** `stdlib/option.tur` and `stdlib/result.tur`
declare real `defdata` sums (`(None)/(Some A)`, `(Ok A)/(Err B)`; tags follow
declaration order), every accessor and instance is match-based, and the whole
inline-C surface -- the codegen preamble helpers (`tur_box_some` / `tur_is_ok`
/ ...), the stdlib layout users (json / schema / serial / safe / panic /
process / future / weak / refined / args / typeclass-show), and the fixture
population -- speaks the tagged monomorph layout `{ int tag; union { ... }
as; }` (16 bytes for both types; `Result` down from 24, the dead arm gone).
The historical none-as-NULL is still accepted on the READ side (`tur_is_some(0)`
is false, and a carrier match reads a NULL scrutinee as tag 0).  **Concrete
monomorphs now flow BY VALUE by default** -- SR2a graduated 2026-08-27, see
SR2c below; a shape the by-value predicate declines (self-recursive, `:heap`,
GADT, fixpoint partner) or an erased generic base still rides the int64
carrier.  The tree-walking interpreter builds and matches ctor-named values
("Some"/"None"/"Ok"/"Err") with the legacy box shapes still readable.
Validation at landing: full compiled suite 2712/0, interpreter suite 1867/0,
sr2-seam 12/12, sr4-seam 24/24.

The payoff phase, and the reason SR1 is worth doing.

```turmeric
(defdata Option [A]   (None) (Some A))
(defdata Result [A B] (Ok A) (Err B))
```

Buys: `Result` 24 -> 16 bytes, the dead-arm write gone (so `E` no longer needs
a zero value), and SR3 becomes possible at all.

~~**Hard prerequisite: SR1.**~~ **Corrected by the gate
([sr2-gate-results.md](sr2-gate-results.md)): SR1 as shipped is NOT the
prerequisite.** SR1 covers non-parametric sums only; Option and Result are
parametric, and a parametric sum monomorph today mallocs and leaks even with
SR1 done.  The real prerequisite is **SR2a -- by-value parametric sum
monomorphs** -- and its prototype seam (`TUR_SR2_APP_SUM_BYVALUE=1`, default
off, provably inert) is in the tree.  The original reasoning still stands one
level down: doing the CONVERSION before SR2a would take the two most-used
types in the language from by-value to heap-allocated-and-leaked.

**Gate verdict (full results in the gate doc):** the direct-use subset --
construct, match, params, returns -- already runs allocation-free under the
seam.  The cost is an 11-fixture worklist (vs SR1's 34) that is ONE family:
the M7 HKT-carrier machinery's assumption that a `(F A)` monomorph rides the
int64 carrier -- which is exactly where Option and Result live (Functor and
Monad instances).  One failure is a SILENT runtime SEGV (`parsec-tutorial`'s
`PRes`, the Option shape verbatim, through parser-combinator closures) -- the
same hidden-by-a-cast signature as the archived fat-dispatch report.  Two
findings sit outside codegen: a nullary ctor with an inferable type argument
does not select its monomorph (so `(none)` in argument position would regress
to annotation-required -- an elaboration fix on the critical path), and
`vec-of` over a parametric sum monomorph ICEs on the DEFAULT path today
([filed](../reported/vec-of-parametric-sum-monomorph-ice.md), predates the SR
work, and `vec<option<T>>` is that shape).

**This one is user-visible**, so per the experimental-features rule in
`CLAUDE.md` it wants an `EXPERIMENTS[]` row with every descriptor field
populated, `experiment_warn_if_used` at the elaboration entry point, and
`plan_path` pointing here.

### SR2c -- the experiment row -- **GRADUATED 2026-08-27**

`--enable=parametric-sum-byvalue` is retired.  `sr2_app_sum_byvalue()` reads a
default-`true` `g_sr2_app_sum_byvalue`; `TUR_SR2_APP_SUM_BYVALUE=0` restores the
carrier for bisection, read once in `main.c` exactly as SR1's is.  The name is
in `GRADUATED[]`, so a lingering `--enable` is a TUR-W0063 no-op for one minor
line.  `tests/run-sr2-seam.sh` and its `tur_sr2_seam` ctest target are retired
with it: the harness existed because nothing in CI compiled a parametric sum by
value, and now every `bash tests/run.sh` compiles all eleven of its fixtures
that way.

**The row's soak had exactly one job** -- do not fix the ABI before SR2b, its
heaviest client, exists -- and SR2b landed in-tree, then across the spices
(`turmeric-spices` #59, "migrate every spice off the pre-sum Option/Result
layout").  `expires_at` was 0.42.0 and nowhere near; graduating early is the
routine case, not the exception.

**The flip cost 21 fixtures, and the gate document could not have predicted
them.** Its "2711/0 under the seam" was measured *before* SR2b, when Option and
Result were still discriminated records -- so the seam only moved user ADTs.
Once the two most-used types in the language are the population, seven distinct
defects surfaced, every one a place where two spellings agreed only because a
parametric sum monomorph rode the carrier and both c-named to `int64_t`:

| defect | where |
|---|---|
| the carrier->by-value readback's NULL guard lived in the pre-SR2b single-ctor RECORD branch, so `(some? (:: (lookup 3) (Option int)))` deref'd `TUR_NONE` | `emit_carrier_bridge`, emit_core.c |
| a match on an erased instance base's param bound the aggregate from an `int64_t` slot (`ap`'s `ff : (Option (fn [a] b))`) | match scrutinee, emit_expr.c |
| the same match resolving its element from a DIFFERENT instantiation than the active spec passes | match scrutinee, emit_expr.c |
| an Option/Result pointer-box payload slot (`tur_adt_ArithError *`) bound as a value | match field binder, emit_expr.c |
| a by-value spec param spilled to a carrier sink at its STATIC type -- the repr-shadow ICE at `arg-bridge` | call arg bridge, emit_expr.c |
| SR1's carrier->by-value-sum arg rule stacking on the poly-wrapper's own unbox, double-deref'ing | call arg bridge, emit_expr.c |
| the catch-unwind GROUP trampoline never set `mem_ret_aggr[]` past member 0, so an aggregate-returning member saved through a scalar cast | `gs_build` caller, emit_fns.c |

Two source-side changes came with it, both of them the CLAUDE.md `:int`
stand-in rule catching up with the representation:

- **`ok?` / `err?` are now `[A B] [r : (Result A B)]`**, match-bodied, like
  `some?` / `ok-val` / `err-val` before them.  They were the last carrier-typed
  Result accessors, and an `:int` parameter stops being a harmless erasure the
  moment the value flows by value -- it becomes a straddle every caller has to
  spill across.  Inline-C carrier producers name their type at the boundary now
  (`(ok? (:: (safe-div 10 2) (Result int int)))`), exactly as they already did
  for `some?`.
- **`arc-weak-upgrade` dropped its manual `option-free` calls.** They were
  added under SR2b to release the carrier box; by value there is no box, and
  the call is a `free()` of a stack slot.

### SR3 -- niche filling -- slice A SHIPPED, slice B UNSHELVED 2026-08-28 (`--enable=option-niche`)

Once `Option` is a real sum, `None` can be represented as the null pointer for
pointer-payload elements, taking `Option<ptr<T>>`, `option<Vec T>`,
`option<Cons T>` from 16 bytes to 8.

**Two of those three examples are wrong, and the gate is how we found out.**
`Option<ptr<T>>` cannot take the niche because a `:ptr<T>` may legitimately be
null; `option<Cons T>` cannot because the empty list already IS 0. Slice B's
section below has the detail.

Plausibly the largest of the three size wins, given how common those shapes are
-- `option<vec<...>>` and `option<Cons ...>` were exactly the shapes the
nested-monomorph fix touched. **Gate:** needs SR2 (done), and needs SR0(a) to
show the volume is there.

**Census (stdlib + fixtures; no spices checkout on the gate box):** the
pointer-payload Option shapes are real but concentrated -- `env` (5 spellings),
`httpd-string` (5), `args` (2), `re` (2), `docstrings` (2), plus the
`(Option String)` / `(Option (Vec ...))` idioms, and ~19 fixture files.
**Corrected 2026-08-30 -- most of that list is not the eligible shape**
([results](../../benchmarks/option-niche-size/RESULTS.md)): `env` / `args` /
`re` are `(Option cstr)` and ineligible, and the `docstrings` rows are string
literals. Emitted, the eligible population is TWO monomorphs across eight
files.
**Result gets no niche at all**: both of its variants carry a payload, so NULL
cannot discriminate `Ok` from `Err` -- SR3 is Option-only.

**Slice A -- nullary `None` as the null carrier -- SHIPPED 2026-08-27, default
on.** SR2b's read side already accepted none-as-NULL everywhere
(`tur_is_some(0)` false, carrier matches read a NULL scrutinee as tag 0), so
a tagged None box whose only content was `tag = 0` was pure allocation.  The
three producers now return the null carrier: the monomorph carrier ctor
(types.c `emit_registered_adt_app_rec`), the generic base ctor
(emit_module.c), and the preamble `tur_none()` -- keyed by
`adt_ctor_is_null_none` (shape-pinned name check, the
`adt_field_is_ros_pointer_box` precedent).  The if-chain match path gained
the same NULL-as-tag-0 guard the switch path had.  This removes the None
allocation for EVERY element type, not just pointers: a 2e6-iteration
`(none)` loop peaks at 10.3 MB RSS where the still-boxing `(some i)` twin
peaks at 64 MB.  It also shrinks the
[carrier-box ownership leak](../reported/carrier-sum-option-boxes-have-no-owner.md)
to Some/Ok/Err constructions only.  Validation: full suite 2712/0, turi
1867/0, sr2-seam 12/12, sr4-seam 24/24.

**Slice B -- `some(p)` carried AS the payload pointer (16 -> 8) -- GATE RUN
2026-08-27 and SHELVED; UNSHELVED 2026-08-28 as `--enable=option-niche`.**
The phase now has its own plan --
[sr3-option-niche-plan.md](sr3-option-niche-plan.md) -- and the original gate is
archived at [sr3-slice-b-gate-results.md](../archive/sr3-slice-b-gate-results.md)
with an errata header.  The env seam is gone; slice B is a registered
`EXPERIMENTS[]` row (prototype, introduced 0.41.0), which is what an in-flight
feature carrying a hand-maintained soundness allowlist should be.  Corpus 2712/0
both with the experiment and without.

**What reversed the shelving is exactly the follow-up the gate named.**
`defopaque` over a pointer now c-names as `void *`
([results](../archive/opaque-pointer-c-spelling-gate-results.md), graduated
2026-08-28), so a niche value is distinguishable from a carrier box and
`String` / `StringBuilder` join the eligibility allowlist -- which is the whole
`(Option String)` census this section enumerates below.  Read the two
disqualifications that follow with that in mind: the `String` one is gone, and
the `Cons` one was assessed 2026-08-28 and stands PERMANENTLY -- declined, not
deferred.  The tree-wide `option<Cons>` population is one fixture that never
wraps an empty list, nil-is-0 is load-bearing in ~60 sites including the
variadic-rest ABI (where moving it breaks user inline-C silently), and the
per-payload-sentinel alternative makes the word 0 mean `Some(nil)` on one side
of a carrier crossing and `None` on the other.  Full pricing in
[sr3-option-niche-plan.md](sr3-option-niche-plan.md), What is left, item 3.

**And it turned up one crossing the first gate could not have reached.** An
inline-C body declared `: (Option String)` builds its result with
`tur_some_ptr`, which returns the CARRIER; a niche consumer then reads the box
pointer as the payload.  No `Vec`/`Map`/`Set` fixture builds an Option in
inline-C -- `String` is the payload people actually do that with -- so the
first gate's "the bridge is ONE chokepoint" was true only of the population it
could see.  Two more rows now: the niche let-binding and the niche call
argument, both routed through the same `emit_carrier_bridge`.

**The recommendation below -- fold slice B into the graduation -- was followed,
and it was half right.** The obstacle it names DID dissolve: after SR2a a
concrete Option consumer specializes, so `some?`/`unwrap` compile against the
niche representation directly and the bridge the plan feared as a long tail is
ONE chokepoint (`emit_carrier_bridge`). The suite found exactly one erased
crossing left -- a typeclass `Eq` dictionary -- and it was a silent wrong
answer until that chokepoint was taught the crossing.

**What shelves it is the population, which neither the plan nor its census
checked for eligibility.** The niche claims 0 for `None`, so a payload type
that has already spent its null cannot take it, and two disqualifications
between them cover the entire census:

- **`Cons`'s nil IS 0** ("At runtime, nil is 0" -- stdlib/list.tur; `(defn tnil
  [] : int 0)`), so `(some (tnil))` would read as `(none)`.
- **`String` c-names to `int64_t`**, as every `defopaque` does whatever its
  declared `:ptr<void>`, so its niche form would be byte-indistinguishable from
  the CARRIER form and every `strcmp(cname, "int64_t")` site would deref the
  payload as a box.

That left `option<Vec T>` / `option<Map ...>` / `option<Set ...>`: **one file
in the tree**, which is why the gate called the actionable follow-up "not slice
B -- it is giving `defopaque` over a pointer a pointer C spelling, which makes
`String` eligible and is the whole census."  That is what happened, and the
`String` half of the paragraph above is now historical: it landed 2026-08-28
and slice B came off the shelf with it.

The original obstacle, as scoped before the gate: The compiled pipeline's default path is semi-erased:
generic bases (`unwrap`, `some?`, every instance-method carrier base) receive
`(Option A)` as one int64 for EVERY `A` and read `->tag` through one shared
layout.  A niche-filled value entering such a base would have its "tag" read
deref the payload pointer's first word -- the representation must be known at
every read site, which only the fully-monomorphized tier guarantees.  So
slice B is confined to the byvalue experiment tier, plus
materialize/dematerialize bridges at every erased crossing (build the tagged
16-byte form when a niche Option passes erased, re-derive on return) -- a new
representation state layered on the bridge family the byvalue path already
maintains.  There is also a semantic edge slice A does not have: a genuinely
NULL pointer payload (`(some (:: 0 :ptr<void>))`) becomes indistinguishable
from `(none)`; the niche is sound only for payload types whose valid values
exclude 0 (`:heap` handles, `String`, fat closures), and nothing in the type
system marks non-nullness today.  **Recommendation: fold slice B into the
`parametric-sum-byvalue` graduation** -- once byvalue is the default, niche
filling is a layout decision inside a monomorph the compiler always sees, and
the erased-crossing bridges are the ones that graduation must harden anyway.
Building it before then doubles the bridge states for an 8-byte win on a
non-default tier.

### SR4 -- recursive sums -- DEFAULT FLIPPED TO BY VALUE 2026-09-02 (RM4)

**Decision: by value is the default.** RM4 in
[reclamation-plan.md](reclamation-plan.md) owned this and re-measured the same
two workloads after RM0 established that no arena is coming (RM2/RM3 have no
constituency), which was the premise for waiting:

| workload | carrier | by value | |
|---|---:|---:|---|
| logic.tur bind+walk, 400k passes, n=8 | 0.49-0.51 s | 0.51-0.52 s | ~1.03x |
| ... peak RSS | 370 MB | 202 MB | 1.8x less |
| re.tur compile+match, 5k passes | 68-90 ms | 65-71 ms | not slower |
| ... peak RSS | 41 MB | 33 MB | 1.2x less |

The time side of the trade shrank to noise while the memory side held, so
the one-line flip was made (`is_self_recursive` in `adt_sr1_sum_candidate`,
types.c). `TUR_SR4_RECURSIVE_CARRIER=1` restores the carrier for A/B
measurement, and `tests/run-sr4-seam.sh` (ctest `tur_sr4_seam`) now keeps
THAT path green with the inverted canary. Full suite 2749/0 under the flip
with no snapshot drift; leak-check 70/0/0; both seams green. The section
below is the pre-flip record.

### SR4 (pre-flip record) -- UNBLOCKED AND MEASURED 2026-08-27; default stayed carrier

**The blocker is fixed.** The fat-dispatch ABI disagreement is
[resolved](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md) --
every fat boundary speaks the b4box convention, spelled once in
`thunk_param_slot_c_name` -- and with `TUR_SR4_RECURSIVE_BYVALUE=1` the FULL
suite is **2708 passed / 0 failed** with recursive sums by value.

**So the admission was measured before being defaulted, and the measurement
says no** (or: not yet). On the two real recursive-sum workloads:

| workload | carrier | by value | |
|---|---:|---:|---|
| logic.tur bind+walk, 400k passes | 0.40s | 0.45s | ~1.13x slower |
| ... peak RSS | 116 MB | 51 MB | ~2.2x less memory |
| re.tur compile+match, 5k passes | 14 ms | 15 ms | ~1.07x slower |

(Re-measured after the SR4-perf constructor-prologue fix, 2026-08-27. The
original measurement was ~1.4x / ~1.35x; profiling the gap found HALF of it
was the by-value ctors' whole-union `{0}` zero-init -- 24-48 bytes of zeros
written and mostly overwritten per construction. The zeroing is now scoped
to the tag pad and the union tail beyond the active variant, which is free
on the widest variant; `emit_byval_ctor_prologue` in emit_module.c carries
the rationale, including why full byte determinism was never an invariant --
flat products never zero-initialized at all. This also cheapens every
NON-recursive sum construction on the default path.)

By value halves the mallocs (the payload no longer boxes; the spine box per
node remains), but each walk step still deref-COPIES a 24-48 byte aggregate
out of the spine box where the carrier copied one word. The remaining ~13% /
~7% is spread thin: the arg-spill and in-place-construction candidates were
each hand-measured at ~2%, so the next real lever is pointer-binding match
fields read out of the spine box (safe today only because the boxes are
never freed -- a borrow that reclamation would invalidate). Instruction
counts and valgrind's cache sim both FAVOR by-value (99M vs 144M Ir, fewer
simulated misses); the residual slowdown is memory-system behavior the sim
does not model, so trust the wall clock here, not the counters. This is the
same lesson as rows B-vs-C above from the other side: the allocation was
never the whole cost.

**What this phase now is:** a one-line default flip, and the trade is now
close (7-13% time for 2.2x memory) --
(`is_self_recursive` in `adt_sr1_sum_candidate`, types.c, where the decision
record lives) waiting on either a workload that wants memory over speed, or
reclamation landing first ([reclamation-plan.md](reclamation-plan.md), RM4
owns this decision) -- an arena makes the carrier's mallocs cheap AND
keeps one-word copies, at which point by-value recursive sums may have no
constituency at all. Measure again then; the seam reproduces everything.

**The seam is CI-armed** (`tests/run-sr4-seam.sh`, ctest target
`tur_sr4_seam`): the recursive-sum fixture population builds and runs under
`TUR_SR4_RECURSIVE_BYVALUE=1` on every CI run, behind a canary that fails
loudly if the seam ever stops biting. So the green state cannot rot silently
-- the flip stays a one-line decision, not a re-excavation -- and the
sanitizer-gate failure mode ("a gate nobody turns on decays to a gate nobody
notices") is closed for this gate specifically.

### SR4 (superseded 2026-08-27) -- as scoped 2026-08-26, one blocker

**Measured, not estimated.** Admitting recursive sums to the by-value path
(drop the `is_self_recursive` test in `adt_sr1_sum_candidate`) leaves the suite
at **2705 passed, 2 failed**, and both failures are a single defect:
[fat-dispatch-wide-byvalue-aggregate-argument](../reported/fat-dispatch-wide-byvalue-aggregate-argument.md).
Every other recursive sum in the tree -- `Term`, `Regex`, `RxCls`, `RxPos`,
`RxStrs`, the fixture trees and lists -- already lowers by value and runs
correctly.

**Three of this phase's premises are now wrong.**

1. **"Field-level boxing" is not the work.** The gate said so and this confirms
   it: the recursive field already rides the carrier, and no phase needs to be
   built to make that happen.
2. **The `logic.tur` rewrite is not the blocker either.** Of the two
   carrier-ascription sites the gate found, one was simply badly typed --
   `fmap-goal-raw` took its callback as a bare `:fn`, so its result was an
   erased word that had to be cast back with `(:: (f s) :Subst)`. Giving it its
   real `(fn [Subst] Subst)` type deleted the reinterpret, and that change is
   landed and green on the default path. The other is not a source problem at
   all -- it is the codegen bug above.
3. **"Reclamation first" no longer gates it.** That ordering existed because
   SR4 was believed to be expensive and worth little against a cheap allocator.
   SR4 is now one codegen fix away, and like SR1 it does not make allocation
   cheaper -- it removes the allocation, which is not a ratio the reclamation
   rows can be compared against.

**What SR4 actually is now:** fix the fat/poly dispatch disagreement, delete the
`is_self_recursive` test, regenerate snapshots. The report has the diagnosis,
a minimal repro for both manifestations, and two candidate fixes already tried
and rejected with the reasons.

Be careful with the second manifestation. Under the carrier the disagreement is
a hard C error; by value it is a **silent wrong answer** (`logic-reify` prints
`0` where `10` is correct, with no diagnostic and no sanitizer report). Any
regression fixture must assert the VALUE, not merely that it builds.

### SR4 (original plan text) -- field-level boxing for recursive sums

The remaining 21 types, `Term` / `Subst` / `Stream` among them. The value
travels by value; only the self-referential field stays a pointer. This is what
`benchmarks/adt-alloc/ceiling.c`'s row C models (`VSubst` keeps `next` as a
pointer), so **1.41x is this phase's number, not SR1's** (re-measured
2026-08-25; it was published as 1.8x).

**Gate:** reclamation first ([reclamation-plan.md](reclamation-plan.md)) --
and on the re-measured numbers that is no longer a sequencing preference
but the substance of the whole thing.

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
