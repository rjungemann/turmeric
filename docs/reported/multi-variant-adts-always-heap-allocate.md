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
   `tests/run-leak-check.sh`, so a bad free is catchable. It stays behind
   `TUR_ADT_SLAB=1` as a measurement seam.
3. **The transformative number needs both** (18x). That is the case for doing
   the by-value ABI work -- but only *after* the allocator, knowing it buys
   1.8x on its own, and knowing (see **Scope**) that the 1.8x requires the
   field-level-boxing variant for recursive sums. The easier two thirds of the
   population -- non-recursive sums -- buys nothing on this workload.

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
