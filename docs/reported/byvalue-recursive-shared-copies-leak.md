# A spine that reaches a consuming callee through a borrow, or lives in a container, leaks

**Severity: low.** Leaks only; every shape that used to be a use-after-free
is closed. Filed while closing
[byvalue-recursive-adt-boxes-are-never-freed](../archive/byvalue-recursive-adt-boxes-are-never-freed.md);
**narrowed 2026-09-28** -- see "What was fixed" below.

## What is left

1. **A consuming callee reached from a borrow discharges nothing, for every
   caller.** Whether a parameter is owned is a whole-program fact (emit_core.c,
   "ownership PROVENANCE"): one call site passing a borrowed copy --
   `(tail2 xs)` with `xs : ^borrow` -- makes the parameter unowned everywhere,
   so the callee's owned callers leak what it would have freed.  The borrowing
   caller's own argument leaks too: `via-borrow` below hands its borrow to a
   callee that neither proves it keeps nothing nor returns only an alias, so
   the caller's local is not confined and is not freed.
2. **A container of by-value recursive values never frees its elements'
   spines.** This is the container ownership model rather than a hole in it: a
   compiled `Vec` local is not freed at scope exit at all (`vec-free` is the
   explicit release, and regions reclaim a non-escaping one), and `vec-free`
   frees the element slots, not what they point at.

Both pinned by `tests/fixtures/byval-recursive-adt-shared-copy-not-freed`
(`known-leak`: 176 bytes in 7 allocations; the gate still fails on a
use-after-free).

```turmeric
(defn tail2 [xs : Lst] : Lst
  (match xs (Cons h t) t (Nil) (Nil)))
(defn via-borrow [^borrow xs : Lst] : int (llen (tail2 xs)))
```

## Fix directions

- For (1): **clone at the borrow**. A `^borrow` (or global, or element) value
  passed to a CONSUMING parameter is a move out of something that keeps
  owning it -- Rust rejects it -- and a deep clone at that call site would keep
  the callee's parameter owned for every caller.  Needs a
  `clone_localowned_<T>` glue beside `drop_localowned_<T>` (recursive fields
  copied box by box, rc fields incremented, `any` fields deep-copied), which
  does not exist yet.  Per-call-site ownership (emitting a non-discharging
  twin of the callee for borrowed callers) is the alternative, and costs a
  variant propagated through the call graph.
- For (2): element drop glue for a `Vec` of a by-value recursive type, once
  `Vec` locals themselves have an owner.

## What was fixed (2026-09-28)

- **An rc-field struct copied out of a shared view was a live use-after-free**
  (the "same hole in another type family" this report used to end on, and it
  was wider than filed): a copy of a `^borrow` returned by value, a global
  returned by value, a `vec-get` element and a match binder of a borrowed ADT
  each had a let-local decrement a count it never took (ASan, all four).  A
  SHARED VIEW -- a `^borrow`, a global, a container element, a binder of one --
  is now cloned where it becomes an owner (a let-local, a function result, a
  `return`): the clone takes +1 per rc field (elab_forms.c,
  `elab_own_byval_copy`).  A `ref` field has no count to take: a let-local that
  may hold a borrowed one leaves it to its owner, and a RESULT that would is
  TUR-E0108 ("cannot move out of a borrow").  `byval-rc-struct-shared-copy`,
  `errors/byval-ref-field-returned-from-borrow`.
- **The source of a copy lent to a `^borrow` callee is freed** (was item 1).
  A `^borrow` recursive-ADT parameter kept nowhere but the result gets a
  result-alias bit (`resalias_param_mask`); the alias walk tracks a local bound
  to such a call as an alias of its argument instead of calling it a hand-off,
  and a walk rooted at a `^borrow` tracks every local bound into the root (none
  of them can be an owner).  `(let [w (id-b zs)] (llen w))` now frees `zs`.
- **A fresh spine lent to a non-retaining callee is freed after the call.**
  `(llen (build 3))` leaked the whole temporary on every call -- no owner at
  all -- and was not in this report; it is the commonest shape of the family.
  The spill temp joins the pending-drop queue that already frees a lent Option
  box.  `byval-recursive-adt-lent-temporary-freed` (300 allocations -> 0).
- The leak gate now builds with the runtime compiled from source
  (`TUR_RUNTIME=source`), so a use-after-free INSIDE the rc runtime is
  instrumented: with the prebuilt archive `rc_strong_decrement` ran
  uninstrumented and the rc-field double decrement passed the gate.
