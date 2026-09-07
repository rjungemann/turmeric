# A by-value recursive ADT leaks one box per link

**Severity: low-medium.** One `malloc` per cons cell of any self-recursive
by-value `defdata`, never freed. Bounded by the structure's size, so it is a
retained-forever cost rather than unbounded growth in a loop -- but a program
that builds and discards lists in a loop does grow without bound.

Split out of
[saffron-any-return-defeats-the-frame-box-rule](../archive/saffron-any-return-defeats-the-frame-box-rule.md)
(now resolved), which measured it while establishing that it was NOT that bug.

## Repro -- no `any` anywhere, plain Turmeric

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn llen [xs : Lst] : int
  (match xs
    (Cons h t) (+ 1 (llen t))
    (Nil)      0))
(defn main [] : int
  (let [xs (Cons 1 (Cons 2 (Cons 3 (Nil))))] (println (llen xs)))
  0)
```

Built with the leak harness's own flags
(`TUR_CC_FLAGS="-O1 -g -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -Lbuild/src"`):

| cells | LeakSanitizer |
|---|---|
| 3 | `72 byte(s) leaked in 3 allocation(s)` |
| 5 | `120 byte(s) leaked in 5 allocation(s)` |

Exactly one box per link, scaling linearly. Each is the heap copy of the
recursive `tl` field: a by-value product cannot ride the int64 carrier, so the
field slot holds a pointer to a `malloc`'d `tur_adt_Lst`.

## Root cause

`AdtDef.needs_drop_glue` is set when a constructor has an `rc`/`ref`/`weak`
field -- an OWNING field in the reference-counted sense. A self-recursive
by-value field is owning in the allocation sense (the parent's slot is the only
pointer to that box) but is not one of those kinds, so no drop glue is emitted
and nothing ever frees the chain.

This is the same *shape* as
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
-- a box the layout requires and the ownership model does not name -- but a
different producer: that one is the Option/Result carrier, this one is any
user `defdata` that names itself.

## Where it shows up

`tests/fixtures/saffron-higher-order` carries a `known-leak` pointing here
(840 bytes in 21 allocations). Its `(Cons [hd : any tl : any])` boxes the tail
through the `any` widen rather than the recursive carrier, so the box is 40
bytes instead of 24 -- but the count is still one per cell and the cause is
identical. Saffron makes the shape easy to reach, it does not create it.

## Fix directions

1. **Extend `needs_drop_glue` to a self-recursive by-value field.**
   `AdtDef.is_self_recursive` already records exactly this property, at
   declaration time, because it cannot be recovered afterwards (a recursive
   field's `CtorField.full_type` is deliberately NULL). The drop glue would walk
   the chain and free each box. The risk is aliasing: two values sharing a tail
   would double-free, so this needs the same freshness question the `any` passes
   answered, or a refcount.

2. **Refcount the link.** Heavier, and it settles aliasing by construction.
   Consistent with what `docs/upcoming/saffron-lang-plan.md` S5 proposed for
   `any` boxes.

3. **Leave it, and say so in the guide.** Defensible for a language where a
   long-lived structure is the normal case, but it should then be a documented
   contract rather than an unremarked cost -- the union/intersection guide took
   that route for the `any` box before the widen passes closed it.

Direction 1 is the smallest and the only one that needs no new runtime concept;
it should be measured against the aliasing case first.
