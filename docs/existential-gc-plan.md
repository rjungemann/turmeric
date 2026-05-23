# Existential Types -- GC Integration Plan

> **Status:** EXG1 shipped; EXG2 partial (via rc_cb_alloc); EXG3 deferred.
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

- [x] **EXG1-1** Defined a combined-allocation layout.  The record and
  its witnesses share one heap block laid out as:
  ```c
  typedef struct tur_existential {
      int64_t  value;
      int32_t  n_witnesses;
      void    *witnesses[];   /* flexible array; length = n_witnesses */
  } tur_existential_t;
  ```
  Total size = `sizeof(tur_existential_t) + n_witnesses * sizeof(void *)`.
  The two-malloc indirection has been removed: there is now one pointer
  to free (and that free happens through the rc subsystem, not the
  pack-site code).  See `src/compiler/emit_module.c` (typedef emit).

- [x] **EXG1-2** Added drop hook `tur_existential_drop(void *p)`.  The
  payload sits inline in the `RcControlBlock` allocation, so freeing the
  block itself (via `free(cb)` in `rc_free_queue_drain`) reclaims
  everything; the witnesses array stores stable pointers into static
  dict singletons and never needs disposal.  The hook is therefore a
  no-op -- it just overrides the default `free(value)` drop so the inline
  payload is not double-freed.  See `src/compiler/emit_module.c`.

- [x] **EXG1-3** Updated `emit_expr.c` `EX_EXISTS_PACK`:
  - Replaced the two `malloc` calls with a single `rc_cb_alloc(
    sizeof(tur_existential_t) + n*sizeof(void*), TY_PTR_VOID,
    tur_existential_drop)` call.  Strong count starts at 1 (default in
    `rc_cb_alloc`).
  - Initializes the payload (value, n_witnesses, witnesses[i] =
    &dict_*_singleton) through the `cb->value` indirection.
  - Returns `(tur_exists_t)cb` (the control block pointer).

- [x] **EXG1-4** Updated `emit_expr.c` `EX_EXISTS_OPEN`:
  - The scrutinee value is the control-block pointer; the payload is
    reached via `((tur_existential_t *)((RcControlBlock *)cb)->value)`.
  - Reads `payload->value` into `v` as before.
  - Inside the `open` body, the scrutinee is a borrow (no count change);
    drop happens at the end of the enclosing scope via EXG1-5.

- [x] **EXG1-5** Wired auto-drop into the let-binding elaborator.  In
  `src/compiler/elab_forms.c` the existing TY_RC auto-drop block was
  generalised to also fire for constrained existentials (`bt.kind ==
  TY_EXISTS && bt.as.forall_.n_constraints > 0`).  This injects a
  `(defer (rc/drop x))` at let-scope exit which the EX_RC_DROP emitter
  lowers to `rc_strong_decrement(...)` + `rc_free_queue_drain(...)`.
  The implicit `void* -> RcControlBlock*` conversion at the call site
  is permitted by C and avoids a dedicated EX_EXISTS_DROP kind.

  Cross-function ownership transfer (pack -> return, pack -> arg) still
  needs an `EX_EXISTS_CLONE` analogue and explicit borrow-check rules;
  v1 closes the leak for local `(let [e (pack ...)] (open e ...))`
  patterns and defers the rest.

- [x] **EXG1-6** Confirmed `EX1c` constraint check unchanged.  Only the
  emit changes; `n_witnesses` and constraint resolution on the AST are
  identical.

- [x] **EXG1-7** Tests:
  - `tests/fixtures/exg1-rc-drop` -- a constrained pack inside a
    `(let ...)` whose scope exits without an `open`.  Verifies the auto-
    drop fires (no leak under ASan).
  - `tests/fixtures/exg1-rc-multi-scope` -- multiple nested constrained
    packs whose lifetimes overlap; all records freed in LIFO order at
    scope exit.
  - `existential-rc-clone` / `existential-rc-return` -- deferred; both
    require cross-scope ownership-transfer plumbing not yet present.
  - `existential-rc-leak-check` -- exercised manually under
    `-fsanitize=address`; both new fixtures run leak-clean.

### Phase EXG2 -- Cycle-collector visibility

- [x] **EXG2-1** Block registration -- inherited from `rc_cb_alloc`.
  Because EXG1 routes pack-site allocation through `rc_cb_alloc`, the
  emitted runtime already calls `gc_register_block(cb)` on every
  existential record.  No additional code needed at the pack site.

- [ ] **EXG2-2** Expose the record's outgoing references to the
  trial-deletion walker.  Today the `value` field is always a scalar
  or pointer-bit-pattern read; `may_contain_cycles` is set to `false`
  in `rc_cb_alloc` when the type kind is `<= 7` (primitives,
  TY_PTR_VOID), which matches the pass we use, so existential records
  are correctly excluded from the cycle walker for now.  Lifting that
  restriction (e.g. for `rc<T>` packed inside a constrained existential)
  requires a layout descriptor the walker can follow, plus a kind tag
  on the control block distinguishing existential payloads.  Deferred.

- [x] **EXG2-3** `gc_unregister_block` is already called from
  `rc_cb_free` / `rc_free_queue_drain` in the emitted runtime; this
  fires automatically when our `tur_existential_drop` runs.

- [ ] **EXG2-4** Cycle-collection test deferred along with EXG2-2 --
  meaningful coverage requires the walker integration above.

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

| ID | Phase | Status | Description |
|----|-------|--------|-------------|
| EXG1-1 | EXG1 | done   | Flexible-array tur_existential_t layout |
| EXG1-2 | EXG1 | done   | tur_existential_drop helper (no-op for inline payload) |
| EXG1-3 | EXG1 | done   | Pack emit -> rc_cb_alloc |
| EXG1-4 | EXG1 | done   | Open emit reads through RC payload |
| EXG1-5 | EXG1 | done   | Auto-drop at let-scope exit (TY_EXISTS w/ constraints) |
| EXG1-6 | EXG1 | done   | Confirm EX1c constraint check unchanged |
| EXG1-7 | EXG1 | done   | RC-flow runtime tests (`exg1-rc-drop`, `exg1-rc-multi-scope`) |
| EXG2-1 | EXG2 | done   | gc_register_block at allocation (inherited from rc_cb_alloc) |
| EXG2-2 | EXG2 | defer  | Expose outgoing refs to cycle walker |
| EXG2-3 | EXG2 | done   | gc_unregister_block in drop (inherited from rc_cb_free) |
| EXG2-4 | EXG2 | defer  | Cycle-collection test (paired with EXG2-2) |
| EXG3-1 | EXG3 | defer  | `:linear` attribute on exists |
| EXG3-2 | EXG3 | defer  | Linear use-exactly-once check |
| EXG3-3 | EXG3 | defer  | Linear emit path (no RC header) |
| EXG3-4 | EXG3 | defer  | Linear-existential tests |

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
