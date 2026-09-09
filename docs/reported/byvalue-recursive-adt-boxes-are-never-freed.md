# A by-value recursive ADT leaks one box per link

**Severity: low-medium.** One `malloc` per link of a self-recursive by-value
`defdata`, never freed.

**PARTIALLY FIXED 2026-09-07.** A non-escaping local's spine is now freed at
scope exit; a local handed to a callee, and every `:copy` recursive ADT, still
leak. Both residues are described below, with what each would take.

Split out of
[saffron-any-return-defeats-the-frame-box-rule](../archive/saffron-any-return-defeats-the-frame-box-rule.md).

## Repro -- no `any` anywhere, plain Turmeric

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn main [] : int
  (let [xs (Cons 1 (Cons 2 (Cons 3 (Nil))))] (println (llen xs)))
  0)
```

| cells | before |
|---|---|
| 3 | `72 byte(s) leaked in 3 allocation(s)` |
| 5 | `120 byte(s) leaked in 5 allocation(s)` |

Exactly one box per link, linear. Each is the heap copy of the recursive `tl`
field: a by-value product cannot ride the int64 carrier and cannot contain
itself inline, so the field slot holds a pointer to a heap copy.

## Root cause

`AdtDef.needs_drop_glue` is set when a constructor has an `rc`/`ref`/`weak`
field. A self-recursive by-value field is owning in the allocation sense (the
parent's slot is the only pointer to that box) but is not one of those kinds, so
no drop glue was emitted and nothing ever freed the chain.

## What was fixed

A DIRECT self-reference (`tl : Lst`, not `(Vec Lst)`) now points the field's
`drop_inner_def` at its own def, which makes the existing by-value drop glue
recursive -- exactly the walk a spine needs -- and a non-escaping local frees
that spine at scope exit via `drop_recspine_<T>(&xs)`, the twin of the
boxed-fn-field drop beside it. `tests/fixtures/byval-recursive-adt-spine-drop`
pins it under the leak harness.

**`:copy` is the soundness line, and it was measured rather than assumed.** Drop
glue makes a type move-only, and that move discipline is what guarantees the
single owner the free depends on:

```turmeric
(let [t (Cons 3 (Nil))  a (Cons 1 t)  b (Cons 2 t)] ...)
```

is already `TUR-E0201: cannot copy unique value 't'`. Under `:copy` the same
program compiles, and the emitted C shows `*__t185 = t` and `*__t187 = t` --
two boxes carrying the SAME tail pointer -- so a per-chain free would free it
twice. `:copy` recursive ADTs therefore keep the leak.

Flipping the flag alone changed nothing across the corpus (2861 passed / 0
failed, no codegen snapshot moved -- none of the 148 snapshot fixtures has a
self-recursive `defdata`).

## Residue 1 -- a local handed to a callee (the common shape)

`(println (llen xs))` marks `xs` moved, so no drop fires and the spine leaks
exactly as before. That is correct as far as it goes -- ownership went to `llen`
-- but nothing discharges it there, and completing the move discipline is
harder than it looks:

A pattern-match binder ALIASES the parent's spine. `(match xs (Cons h t) ...)`
gives `t` a pointer into `xs`'s box chain, and `llen` passes it to its own
recursive call. If a callee freed its by-value ADT parameter at scope exit, that
recursive call would free a sub-chain the outer frame also owns -- a double free,
not a leak. So parameter-side discharge needs to distinguish an owned argument
from a borrowed interior pointer, which the current move tracking does not.

Measured coverage as it stands: a local consumed by an inline `match` or field
read within its own scope is freed (the fixture); a local passed to a helper is
not.

## Residue 2 -- `:copy`, where regions already answer

Most recursive types in the stdlib are `:copy` -- `Term`, `Subst` and `Stream`
in `logic.tur` (the workload `docs/archive/regions-plan.md` was priced on),
`Regex`, `RxCls`, `RxPos` -- so this residue is the larger one by usage.

It already has a working answer: inside a `with-region` bracket the whole spine
is reclaimed on rewind. Verified -- the 3-cell repro above wrapped in
`(with-region (fn [] : int ...))` reports zero leaks. R4 of the regions plan
routed this exact allocation site (`emit_expr.c`'s recursive-ctor-field box) for
that purpose.

## What is NOT this bug

`tests/fixtures/saffron-higher-order` was originally marked against this report.
That was wrong, and measuring the emitted C is what showed it: its `Lst` is
`(Cons [hd : any tl : any])`, so the tail is an `any` box from
`elab_coerce_to_any`'s by-value widen, not the recursive-carrier box at all --
11 widen sites, ZERO recursive-carrier sites. Same family (a box inside a
structure with no owner), different producer, different fix. Filed separately as
[any-widen-stored-in-an-adt-field-has-no-owner](any-widen-stored-in-an-adt-field-has-no-owner.md).

## Fix directions for the residue

1. **Parameter-side discharge**, with a borrow/own distinction for by-value ADT
   arguments. The `nonretain_ptr_param_mask` family answers an adjacent question
   already, but its `_is_ptr_scalar` gate does not admit an ADT parameter, and
   the interior-pointer case above is the part it does not model.
2. **Refcount the link.** Settles both residues, including `:copy`, by
   construction. Heavier, and it prices every construction.
3. **Leave `:copy` to regions, and say so in the guide.** Defensible -- the
   mechanism exists, is measured, and is what the regions plan intended -- but it
   should be a documented contract rather than an unremarked cost.
