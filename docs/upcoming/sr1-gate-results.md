---
title: SR1 prototype gate -- results
category: Planning
description: What happened when a non-recursive multi-variant sum was forced by value behind a compile-time seam -- three crossings fixed, 33 fixtures still red, one silent-miscompile class found, and two of the SR plan's premises corrected.
---

# SR1 prototype gate -- results

The gate specified in [sum-representation-plan.md](sum-representation-plan.md)
SR1: force one non-recursive sum by value behind a seam and measure, before
writing any of it.

**Verdict: SR1 is feasible and bigger than the plan assumed -- but the cost is
not where the plan put it.** The codegen crossings are tractable and cluster
tightly. The real obstacle is that library source depends on sums being int64
carriers.

## The seam

`TUR_SR1_SUM_BYVALUE=1` (env-only, following the `TUR_ADT_SLAB` precedent --
a measurement seam, not a shipping feature). It admits a multi-variant sum to
the by-value path when it is non-parametric, non-GADT, non-heap, and all its
variants' fields are by-value-able.

**Default off, and provably inert:** zero snapshot drift across every
`expected.c`, and `bash tests/run.sh` is **2696 passed, 0 failed** -- the
unchanged baseline.

The structural change is one line of intent: `adt_is_byvalue_product_d`
required `adt_is_flat_product`, so **"flows by value" and "has no tag" were the
same question**. Separating them is the whole of SR1. `adt_is_flat_product`
still reports false for these, so the tagged-union typedef, the tag store and
the tag test in `match` all stay -- only the ABI moves.

## Crossings fixed (3)

1. **`match`'s switch path** (`emit_expr.c`). The if-chain path already had a
   by-value arm; the switch path was carrier-only, because a by-value ADT could
   not previously carry a tag. It emitted
   `tur_adt_Color *__scrut = (tur_adt_Color *)(intptr_t)(c);` against an
   aggregate. Fixed by binding a pointer to a local copy, so all ten
   `__scrut->` field reads below keep working unchanged -- with the
   whole-scrutinee var binding special-cased to copy rather than hand out a
   stack address.

2. **The by-value ctor emitter never stored a tag** (`emit_module.c`, 2 sites).
   Its comment stated the invariant SR1 breaks: *"no tag (byval implies
   single-variant flat product)"*. **This produced silently wrong answers, not
   a build error** -- `ctor_Green()` returned an uninitialised struct and the
   probe printed `red` / `3` where `green` / `12` was correct. Fixed with a
   conditional tag store and zero-init, mirroring the two boxed branches.

3. **The predicate is reached mid-definition** (`types.c`). A self-recursive
   `(Node :Tree :Tree)` queries `Tree` while `Tree`'s ctor array is still being
   filled, so `def->ctors[ci]` can be NULL -- an ASan SEGV. Now declines rather
   than guessing.

## Two premises of the plan, corrected

**1. Recursive sums are NOT blocked by infinite size.** The plan says
`(TPair :Term :Term)` "has no finite inline size" and so needs field-level
boxing (SR4) as a separate, larger change. That is wrong for Turmeric: a
recursive ADT field *already* rides the int64 carrier, so the type has a fixed
inline size and lowers by value today. A self-recursive two-variant `Tree`
compiles and runs correctly under the seam:

```turmeric
(defdata Tree :copy (Leaf :int) (Node :Tree :Tree))
```

emits `ctor_Node(int64_t _0, int64_t _1)` returning `tur_adt_Tree` by value, and
prints the right answer. **Field-level boxing is not a prerequisite -- it is
what the carrier already does.** The plan's SR1/SR4 split, and its claim that
row C's 1.8x prices "the harder variant", both need revisiting.

**2. There is a source-level blocker the plan did not anticipate.** The
`logic-*` fixtures fail with a real diagnostic, not a cc error:

```
stdlib/logic.tur:70:16: error [TUR-E0295]: cannot reinterpret by-value
aggregate 'Stream' as a one-word carrier (:int / :ptr<void>)
```

from source that reads:

```turmeric
(st-append (:: (f v) :Stream) (st-bind rest f))
```

`f` returns a tyvar erased to the carrier, and the ascription reinterprets it
back. With `Stream` on the carrier that is a no-op cast; by value it is a real
representation change, and the compiler correctly refuses. The `reinterpret` is
compiler-inserted -- the word appears nowhere in `logic.tur`.

**So SR1 is not only a codegen change.** Polymorphic library code that erases
to the carrier and ascribes back to a sum has to be rewritten (box through
`any`, per the diagnostic) or covered by a by-value-aware specialization. That
is a different and less mechanical kind of work than fixing crossings.

## Remaining damage

**33 fixtures**, against a 2696-green baseline. They cluster tightly -- five
error shapes, one dominant:

| first error | fixtures |
|---|---:|
| `aggregate value used where an integer was expected` | 9 |
| `incompatible type for argument ...` | 4 |
| `invalid type argument of '->'` | 3 |
| `invalid initializer` | 2 |
| `unknown type name 'tur_...'` | 1 |

(19 of 33 report a first error through `tur build`; the rest are codegen
snapshot mismatches or emitted-C warnings.)

Every one is the byval-to-carrier bridge family -- the same machinery
(`emit_carrier_bridge`, `emit_type_is_byvalue_adt`, `type_uses_carrier_abi`)
that the nested-monomorph fix had to teach about a new case. Only 14 sites in
the compiler consult `adt_is_flat_product` at all, and most are legitimate
tag/layout decisions, so this is not a long tail of independent problems.

## Cost estimate

Bigger than the nested-monomorph fix (8 fixtures, 4 predicates, one session)
but the same *kind* of work for the codegen half: call it 5 predicate families
and a week. The stdlib rewrite is the unknown, and it is the part worth scoping
before committing -- `logic.tur` is the module that most depends on the carrier
representation and also the one the allocation numbers came from.

## Recommendation

Unchanged from SR0: **do not start SR1 for performance.** SR0(a) found real code
barely constructs sums, and this gate adds that the one module which does
(`logic.tur`) is also the one that would need source changes to allow it.

The seam stays in the tree, default off, as the instrument for the next person
who asks. Flipping it on is one env var and the failure list above is the
worklist.
