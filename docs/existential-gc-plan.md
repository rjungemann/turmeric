# Existential Types -- GC Integration Plan

> **Status:** Draft Plan -- follow-on to EX1e
> **Last Updated:** 2026-05-23
> **Type:** Runtime / Memory Management

---

## Overview

Phase EX1e introduced a heap-allocated record for constrained existentials:

```c
typedef struct tur_existential {
    int64_t  value;
    int32_t  n_witnesses;
    void   **witnesses;
} tur_existential_t;
```

`pack` emits one `malloc` for the record and one for the witness array.
`open` reads `record->value` and (in the future) reads through
`record->witnesses[i]` for method dispatch.

**Nothing frees these allocations.** The EX1e implementation deliberately
deferred this to keep the change minimal; the records leak. For
short-lived test programs this is invisible, but production use --
existentials in long-running services, in iterative `pack`/`open` loops,
or in heterogeneous collections -- will leak unboundedly.

This plan defines the work needed to put `tur_existential_t` records
under Turmeric's existing memory-management discipline so they are freed
when no longer reachable.

---

## Current State

| Allocation | Lifetime | Freed by |
|------------|----------|----------|
| `tur_existential_t` record | from `pack` site to program exit | nothing |
| `void **witnesses` array | from `pack` site to program exit | nothing |
| Witness dictionaries (`dict_*_singleton`) | static, file-scope | n/a (static storage) |

The `dict_*_singleton`s themselves are static globals -- not heap-allocated
-- so the witnesses array only stores stable pointers into the data
segment. We do **not** need to track those for collection. Only the
record and the witnesses array need disposal.

### Why existentials don't already work like `rc<T>`

`rc<T>` values carry an `RcControlBlock` header that the runtime increments
and decrements via `EX_RC_OF` / `EX_RC_CLONE` / `EX_RC_DROP` and the
borrow-check pass. Existential packs do not currently route through the
RC pass: there is no `EX_EXISTS_PACK` hook in `borrow_check.c`, no
`rc_drop` at the end of an `open` scope, and `tur_existential_t` has no
control-block header.

### Borrow-check interaction

Because the record does not participate in the borrow checker, passing a
packed existential through several scopes today is borrow-trivial: it
looks like an opaque `tur_exists_t` (a void pointer) with no ownership
semantics. Whatever discipline we adopt has to be expressible in the
borrow checker without forcing every existential-using function into
linear plumbing.

---

## Design Options

Four approaches, ordered by complexity.

### Option A -- Arena per scope (no GC; cheapest)

`pack` allocates the record from an arena tied to the enclosing function
(or, conservatively, the program). Records live as long as the arena;
the arena is freed at function exit.

**Pros:** zero per-record cost, no borrow-check changes, no cycle
concerns. Already a well-trodden Turmeric pattern (the elaborator runs
on arenas).

**Cons:** records that outlive the function -- e.g. a packed value
returned to the caller -- are still alive when the arena is freed
(use-after-free). Mitigated only by escape analysis or by promoting
returned records to a longer-lived arena. Both make the scheme much
more complex than it first appears.

**Verdict:** works for narrow patterns (`(let [e (pack ...)] (open e ...))`
inside one function) but cannot support `pack` results that flow out of
the constructing function. Existentials whose whole point is to hide a
type and be passed around lose their purpose under this scheme.

### Option B -- RC control block per record (recommended)

Allocate the record as the payload of an `RcControlBlock`, with strong
count starting at 1 at the `pack` site. Each `open` borrows the record
(no count change); the `pack` expression's binding goes through normal
RC drop at scope exit, freeing the record when its strong count reaches
zero.

The witness array can be allocated in the same `RcControlBlock` payload
(extend `tur_existential_t` to a flexible-array layout) so a single
allocation covers both. The control block's drop function frees the
combined block.

**Pros:** uniform with the rest of Turmeric's RC discipline; flows
through function returns and through stored data without special cases;
borrow checker already handles `rc<T>` returns.

**Cons:** needs a drop function (small), a new `rc_cb_alloc` call shape
at the `pack` emit site, and an entry in `borrow_check.c` so an
`EX_EXISTS_PACK` value is treated as an owning reference. Cycles
involving existentials (existential carrying a closure that captures the
existential) need the cycle collector -- the records would need to be
visible to `gc_register_block` and to expose their internal references
to the trial-deletion walker.

**Verdict:** the right long-term answer; matches the rest of Turmeric's
memory model.

### Option C -- Linear discipline (linear by construction)

Treat `tur_existential_t` as `:linear` so the type system requires
exactly one consumer (an `open`). The record is freed inside `open`'s
generated code after the body completes.

**Pros:** zero RC cost at runtime; static guarantee of single-use;
matches `lref<T>` semantics already in the codebase.

**Cons:** breaks idioms where the same packed value is opened multiple
times (e.g. a list of existentials iterated twice). Most usefully-typed
existentials are not linear, so this restricts the feature meaningfully
for what is otherwise a flexible-typing tool.

**Verdict:** appropriate for a narrow `:linear`-marked existential
variant; not the default.

### Option D -- Conservative cycle collection (GC-only, no RC)

Register every `tur_existential_t` record with the Bacon-Rajan cycle
collector via `gc_register_block` and skip RC entirely. Records are
freed at the next collection cycle that finds them unreachable.

**Pros:** no per-borrow cost, no need to thread ownership through the
borrow checker.

**Cons:** existentials become unique among Turmeric values in not
participating in RC. Diagnostics, drop ordering, and `:linear`
interaction all become inconsistent with the rest of the language.
Memory pressure rises until the next collection.

**Verdict:** worse uniformity than Option B without major
implementation savings.

---

## Recommended Path

Adopt **Option B (RC control block)** as the default, and reserve
**Option C (:linear existentials)** as an opt-in for callers that want
the runtime cost erased.

| Phase | Approach | Prerequisite |
|-------|----------|--------------|
| EXG1 | Option B -- RC-managed `tur_existential_t` | EX1e shipped (done) |
| EXG2 | Cycle-collector visibility for existentials | EXG1 |
| EXG3 | Optional `:linear` existential variant | EXG1, substructural infra (-Xsubstructural) |

EXG1 closes the leak in the common case. EXG2 makes the discipline
robust against cycles. EXG3 is a refinement for performance-sensitive
callers.

---

## Detailed Implementation Tasks

### Phase EXG1 -- RC-managed existential records

- [ ] **EXG1-1** Define a combined-allocation layout. The record and its
  witnesses share one heap block laid out as:
  ```c
  typedef struct tur_existential {
      int64_t  value;
      int32_t  n_witnesses;
      void    *witnesses[];   /* flexible array; length = n_witnesses */
  } tur_existential_t;
  ```
  Total size = `sizeof(tur_existential_t) + n_witnesses * sizeof(void *)`.
  Replace the current `void **witnesses` indirection so there is one
  pointer to free, not two.

- [ ] **EXG1-2** Add a drop function `tur_existential_drop(void *p)`
  registered with the RC subsystem.  Bodies of constrained existentials
  are scalars (no nested owning fields), so the drop is just `free(p)`
  for the combined block.

- [ ] **EXG1-3** Update `emit_expr.c` `EX_EXISTS_PACK`:
  - Replace the two `malloc` calls with a single `rc_cb_alloc(payload_size,
    TY_PTR_VOID, tur_existential_drop)` call.
  - Initialize the payload (value, n_witnesses, witnesses[i] = &dict_*_singleton).
  - Emit `rc_strong_increment(cb)` on the returned control block so the
    pack expression has strong count = 1.
  - Return `(tur_exists_t)cb` (the control block pointer, which carries
    the payload immediately after the header).

- [ ] **EXG1-4** Update `emit_expr.c` `EX_EXISTS_OPEN`:
  - The scrutinee value is the control-block pointer; the payload is at
    `(tur_existential_t *)((char *)cb + RC_CB_HEADER_SIZE)`.
  - Read `payload->value` into `v` as today.
  - Inside the `open` body, the scrutinee is a borrow (no count change);
    drop the scrutinee at the end of the `open` only when the `open` is
    the consuming use (see EXG1-5).

- [ ] **EXG1-5** Borrow-check pass.  Add `EX_EXISTS_PACK` and
  `EX_EXISTS_OPEN` to `borrow_check.c`:
  - `EX_EXISTS_PACK` produces an owning reference (`rc<tur_existential>`),
    increments the binding's "owning" count.
  - `EX_EXISTS_OPEN` borrows the scrutinee; the scrutinee's drop runs
    at the end of the enclosing scope as for any owning reference.
  - Passing a packed existential as a function argument or storing it in
    a struct increments the strong count (like `rc/clone` does today).

- [ ] **EXG1-6** Update `EX1c` constraint check.  The `n_witnesses` set
  on the AST node is already correct; only the emit changes.

- [ ] **EXG1-7** Tests:
  - `existential-rc-drop`: a constrained pack inside a `(let ...)` whose
    scope exits without an `open`.  Verify the control-block strong count
    reaches zero and the drop function runs (assert via a counter).
  - `existential-rc-clone`: pass a packed existential to two consumers;
    confirm both can open it and the record is freed after both scopes
    exit.
  - `existential-rc-return`: a function returns a packed existential to
    its caller; the caller opens it; record is freed after the caller's
    scope.
  - `existential-rc-leak-check`: run under `-fsanitize=address`; no leaks
    reported (existing `tests/check-span-unknown.sh` style runner).

### Phase EXG2 -- Cycle-collector visibility

- [ ] **EXG2-1** Register each new `tur_existential_t` record with
  `gc_register_block` at allocation time so the cycle collector can
  reach it.

- [ ] **EXG2-2** Expose the record's outgoing references to the
  trial-deletion walker.  The `value` field can be a packed pointer
  (e.g. an rc<T> stored as int64_t); the walker needs to know to follow
  it.  For v1, restrict cycle-collected existentials to ones whose body
  type is itself a heap-managed type (`rc<T>`, `lref<T>`, `:ptr<void>`
  carrying RC); document the restriction in `gc.h`.

- [ ] **EXG2-3** Add `gc_unregister_block` to the drop function so the
  collector does not retain freed records in its suspect buffer.

- [ ] **EXG2-4** Tests:
  - `existential-rc-cycle`: pack a value whose body contains an rc back
    to the existential itself (constructed via mutation); confirm
    `gc_force` reclaims both.

### Phase EXG3 -- Optional `:linear` existential variant

- [ ] **EXG3-1** Accept a `:linear` attribute on the existential type
  form: `(exists :linear [a] [(C a)] T)`.  Pass the attribute through
  to `TY_EXISTS` (extend the `forall_` struct).

- [ ] **EXG3-2** Type-check that linear existentials are consumed exactly
  once: each `pack` produces a linear binding, each `open` consumes it.
  Reuse the existing `SK_LINEAR` infrastructure (`borrow_check.c`,
  `lifetimes.c`).

- [ ] **EXG3-3** Emit the linear path: no RC header, plain `malloc` at
  pack, plain `free` at the end of `open`'s body.

- [ ] **EXG3-4** Tests:
  - `existential-linear-ok`: pack, open once; freed.
  - `errors/existential-linear-unused`: pack, never open -- diagnostic.
  - `errors/existential-linear-double-open`: pack, open twice -- diagnostic.

---

## Task Summary

| ID | Phase | Description |
|----|-------|-------------|
| EXG1-1 | EXG1 | Flexible-array tur_existential_t layout |
| EXG1-2 | EXG1 | tur_existential_drop helper |
| EXG1-3 | EXG1 | Pack emit -> rc_cb_alloc |
| EXG1-4 | EXG1 | Open emit reads through RC payload |
| EXG1-5 | EXG1 | Borrow-check EX_EXISTS_PACK / EX_EXISTS_OPEN |
| EXG1-6 | EXG1 | Confirm EX1c constraint check unchanged |
| EXG1-7 | EXG1 | RC-flow runtime tests + AddressSanitizer leak check |
| EXG2-1 | EXG2 | gc_register_block at allocation |
| EXG2-2 | EXG2 | Expose outgoing refs to cycle walker |
| EXG2-3 | EXG2 | gc_unregister_block in drop |
| EXG2-4 | EXG2 | Cycle-collection test |
| EXG3-1 | EXG3 | `:linear` attribute on exists |
| EXG3-2 | EXG3 | Linear use-exactly-once check |
| EXG3-3 | EXG3 | Linear emit path (no RC header) |
| EXG3-4 | EXG3 | Linear-existential tests |

---

## Relation to Other Plans

- **`existential-types-plan.md`** -- EX1e shipped the heap layout and
  pack-site allocation but explicitly deferred freeing; this plan picks
  up that thread.
- **`refinement-types-plan.md`** -- refinements interact at the open
  boundary: a refinement narrows the hidden type's range but does not
  change the storage discipline.  Independent of GC integration.
- **`typed-collections-plan.md`** -- typed collections (`Vec[T]`, etc.)
  will store existentials when their element type is itself an
  existential; the RC discipline from EXG1 lets a `Vec[Showable]` free
  its elements through the normal vec drop without special-casing.
