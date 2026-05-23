# Existential Types -- GC Integration, Follow-up Plan

> **Status:** EXG4 / EXG5 / EXG6 all shipped, with the cross-plan
> followups (F1-1, F1-2, F1-3, F2-1) closing the originally deferred
> EXG4-5, EXG5-5, and EXG6-5 fixtures and the smart drop hook for
> RC payloads.  EXG4-3 retired (F1-3).  Remaining open: the full
> cycle-construction `exg5-exists-cycle` and `exg4-pack-into-struct`
> are both blocked on the same `defstruct` compound-annotation
> parser extension; track that as a separate `defstruct` work item.
> **Last Updated:** 2026-05-23
> **Type:** Runtime / Memory Management

---

## Overview

EXG1 closed the leak for the *local* pack/open pattern:

```turmeric
(let [e (pack 5 (exists [a] [(Show a)] a))]
  (open e [_ v] ...))           ; e is auto-dropped at scope exit
```

What it does **not** yet handle:

1. **Cross-scope flow.**  A packed existential returned from a function,
   passed as an argument, or stored in a struct must keep the underlying
   `RcControlBlock` alive past the originating scope.  Today the
   originating let auto-drops unconditionally, so any of these patterns
   crash with use-after-free.
2. **Cycles through existential payloads.**  EXG1 sets
   `may_contain_cycles = false` on existential records (because the
   `value` field is a scalar bit-pattern by default).  An existential
   whose hidden type is itself an `rc<T>` can therefore participate in
   a cycle that the trial-deletion walker cannot see.
3. **Linear-discipline opt-out.**  Some callers want the runtime cost
   of RC erased entirely.  A `:linear` annotation on `(exists ...)`
   should fall back to plain `malloc`/`free` with a static guarantee of
   single use.

This plan defines three sequential phases that close each gap.

| Phase  | Approach                                              | Prerequisite |
|--------|-------------------------------------------------------|--------------|
| EXG4   | Cross-scope ownership (clone on share, move on return) | EXG1 shipped |
| EXG5   | Cycle-walker visibility for existential payloads      | EXG4         |
| EXG6   | `:linear` existential variant                         | EXG1 shipped, -Xsubstructural |

EXG4 unblocks every practical use of existentials that flows through
function boundaries.  EXG5 makes RC discipline robust against cycles
formed via packed `rc<T>` payloads.  EXG6 is independent of EXG4/EXG5
and can land in parallel once substructural infra is on by default.

---

## Phase EXG4 -- Cross-scope ownership

### Current behaviour

`elab_forms.c` injects `(defer (rc/drop e))` for every let-bound
constrained existential, gated only on `is_moved` / `is_binding_consumed`.
Neither flag fires for:

- `(defn ... (pack ...))`              -- return value escapes the fn body
- `(some-fn (pack ...))`               -- pack flows into a call arg
- `(let [e (pack ...)] (foo e) e)`     -- e is returned from the let
- `(set! struct.field (pack ...))`    -- pack flows into a struct slot

Each of these produces an aliased pointer that the originating scope
will still try to drop.

### Design

Treat constrained `TY_EXISTS` values like `rc<T>` does today: explicit
`(rc/clone ...)` at every share point, an implicit move when a binding
flows into a return/arg/store position.

#### EXG4-1 -- Move tracking for existential bindings

Extend `is_binding_consumed` (or add a parallel `is_existential_moved`)
to detect:

- The binding appears as the *direct* value of an `EX_RETURN`.
- The binding appears as a *direct* argument to a function call
  (`EX_CALL`), without an enclosing `rc/clone`-style share.
- The binding appears as the source of an `EX_SET` whose target is
  a struct field, a `set!` of a mutable binding, or a `ref/of`.

When detected, set the binding's `is_moved` flag (same field rc<T>
uses) so the existing auto-drop loop skips it.

#### EXG4-2 -- Clone form for existentials

Introduce `(exists/clone e)` (or reuse `rc/clone` if its type-check is
relaxed to accept constrained TY_EXISTS).  The expression lowers to
`rc_strong_increment(cb)` and returns the same control-block pointer.

Borrow-check rule: cloning a moved binding is an error (same diagnostic
as `rc/clone` on a moved binding).

#### EXG4-3 -- Implicit clone at storage sites

Three places need an automatic insertion of `exists/clone` if move
analysis would otherwise leave a dangling pointer:

- **Struct field store** -- `(set! s.field e)` when `field`'s declared
  type is the same existential as `e`'s type.  Inject a clone so both
  the binding `e` and the field point at a counted reference.
- **Vec/HAMT insert** -- same as above; the collection is the new owner
  and the originating binding keeps its drop.
- **Async/spawn capture** -- a packed existential captured by an
  `EX_ASYNC` body must clone for the async side; the caller's drop fires
  normally.

These three sites mirror what rc<T> already does in `elab_structs.c`,
`elab_collections.c`, and `elab_async.c`; the change is to extend the
type checks to also fire on constrained TY_EXISTS.

#### EXG4-4 -- Return-value semantics

If a let binding flows directly to the function's return expression
(`(defn ... [...] (let [e (pack ...)] e))`), mark `e` as moved before
running the auto-drop pass.  The caller's let binding for the return
value owns the resulting control block; the callee's let does not drop.

If the return expression is `(rc/clone e)` (explicit), no move is
recorded -- both sides hold a reference and both drop normally.

#### EXG4-5 -- Tests

- `exg4-pack-return`: `(defn make [] (pack ...))` + caller opens; both
  scopes drop their reference; the record is freed exactly once.
- `exg4-pack-into-fn`: `(consume (pack ...))` where `consume` takes a
  constrained existential by value; clone inserted at call site.
- `exg4-pack-into-struct`: store a packed existential into a struct
  field, observe both the field's drop and the originating binding's
  drop fire without double-free.
- `errors/exg4-pack-double-move`: returning the same binding from both
  branches of an `if` should diagnose like `rc<T>` does.

---

## Phase EXG5 -- Cycle-collector visibility for existential payloads

### Current behaviour

`rc_cb_alloc` sets `cb->may_contain_cycles = false` when the value type
kind is `<= 7` (primitives).  EXG1 passes `TY_PTR_VOID` (= 6) for every
existential record, which is correct *for now* (the value field is a
scalar bit-pattern), but wrong if the body type is `rc<T>` or carries
an embedded RC reference: a cycle through the payload becomes invisible
to the Bacon--Rajan trial-deletion walker.

### Design

#### EXG5-1 -- Per-block kind tag

Add a one-byte tag to `RcControlBlock` (currently `reserved[6]`)
identifying the value layout:

- `RCK_OPAQUE` (0)             -- value is a scalar / bit pattern
- `RCK_EXISTENTIAL` (1)        -- value points at a tur_existential_t
- `RCK_STRUCT_WITH_RC` (2)     -- value is a struct that carries RC fields
- ...

Existing call sites (rc/of, ref/from-rc, etc.) write `RCK_OPAQUE` so
behaviour is unchanged.  EXG1's pack site writes `RCK_EXISTENTIAL`.

#### EXG5-2 -- Walker dispatch

`gc.c`'s trial-deletion walker grows a switch on the new tag.  For
`RCK_EXISTENTIAL`, the walker reads `n_witnesses` from the inline
record (witnesses themselves are static singletons -- not followed) and
inspects the `value` field according to a *payload descriptor* attached
to the AST node at pack time:

```c
typedef struct ExistsPayloadDesc {
    uint8_t kind;          /* RCK_OPAQUE, RCK_RC, RCK_REF, ... */
    /* extend later for struct-with-RC payloads */
} ExistsPayloadDesc;
```

For payload kind `RCK_RC`, the walker treats `((int64_t)value)` as a
`RcControlBlock*` and recurses.  For `RCK_OPAQUE` it stops (current
behaviour, preserved).

#### EXG5-3 -- Pack-site descriptor

`emit_expr.c` `EX_EXISTS_PACK` writes the payload descriptor (size:
one byte) into the control block alongside the kind tag.  The
descriptor is derived from the *concrete* value type at the pack site
-- e.g. `(pack r (exists [a] [(C a)] a))` where `r : rc<int>` writes
`RCK_RC`; the same form with `r : int` writes `RCK_OPAQUE`.

#### EXG5-4 -- Enable `may_contain_cycles` for non-opaque payloads

`rc_cb_alloc` is taught (via a new variant `rc_cb_alloc_kinded`) to
accept the kind tag explicitly so `may_contain_cycles` is only forced
to `false` for `RCK_OPAQUE`.

#### EXG5-5 -- Tests

- `exg5-rc-in-exists`: pack an `rc<int>`, drop the originating
  binding; `gc_force` reclaims both.
- `exg5-exists-cycle`: pack an `rc<Cell>` where the cell holds back a
  reference (via mutation) to the existential; observe the collector
  break the cycle.

---

## Phase EXG6 -- `:linear` existential variant

### Motivation

`pack`/`open` allocate, increment, decrement.  Many existentials are
used once (a single `open` immediately after the `pack`) -- the
RC overhead is pure waste for those.  A `:linear` annotation lets a
caller opt into a no-RC discipline backed by the substructural type
system.

### Design

#### EXG6-1 -- Attribute syntax

```turmeric
(pack 5 (exists :linear [a] [(Show a)] a))
```

The `:linear` keyword on `exists` propagates to the `TY_EXISTS` node:

```c
struct {
    /* ... existing fields ... */
    bool is_linear;     /* EXG6: linear discipline; pack/open exactly once */
} forall_;
```

#### EXG6-2 -- Substructural check (use exactly once)

Reuse `LT1`'s linear-binding machinery (`is_linear`, `is_linear_consumed`,
diagnostics `TUR_E0100_LINEAR_DROPPED`).  Mark the let binding for a
`:linear` pack as `is_linear`; `open` is the unique legal consumer and
marks it consumed.  Two opens diagnose like double-use of a linear
ref; zero opens diagnose like a dropped linear binding.

#### EXG6-3 -- Emit path

For `:linear` packs, bypass `rc_cb_alloc` entirely:

- Pack: `malloc(sizeof(tur_existential_t) + n*sizeof(void*))`,
  populate, return the raw pointer.
- Open: read the record as before; emit `free(record)` at the end of
  the open body (statically, since the body *is* the only use).

No RC counts, no defer thunks, no `rc_free_queue_drain`.

#### EXG6-4 -- Interaction with EXG4/EXG5

A `:linear` existential is fixed-lifetime (pack -> open) and cannot
escape its scope (the substructural check rejects return/store/clone),
so EXG4 and EXG5 do not apply.

#### EXG6-5 -- Tests

- `exg6-linear-ok`: pack, open once; freed; ASan clean.
- `errors/exg6-linear-unused`: pack, no open -- `linear value dropped`.
- `errors/exg6-linear-double-open`: pack, two opens -- `linear value
  used twice`.
- `errors/exg6-linear-escape`: returning a `:linear` packed value from
  a `defn` is rejected at the type-check.

---

## Task Summary

| ID     | Phase | Description |
|--------|-------|-------------|
| EXG4-1 | EXG4  | Move tracking for existential bindings (return / arg / store) -- **shipped (let-tail propagation)** |
| EXG4-2 | EXG4  | `(exists/clone e)` form, lowers to `rc_strong_increment` -- **shipped (rc/clone relaxed to accept constrained existentials)** |
| EXG4-3 | EXG4  | Auto-clone at struct/vec/async storage sites -- **retired** (cross-plan-followups F1-3): the rc<T> baseline requires explicit `rc/clone` at storage sites, so constrained existentials follow the same explicit-clone discipline.  Users wanting to store a packed existential in a struct field or vec slot must call `rc/clone` at the storage site.  If the project ever switches rc<T> to auto-clone semantics, EXG4-3 should be revisited at the same time. |
| EXG4-4 | EXG4  | Return-value move semantics -- **shipped (let-tail propagation covers fn-return-via-let)** |
| EXG4-5 | EXG4  | Cross-scope-flow runtime tests -- **shipped (mostly):** `exg4-pack-let-tail`, `exg4-exists-clone`, `exg4-pack-share` from the original wave, plus `exg4-pack-return`, `exg4-pack-into-fn` and the supporting `ex-exists-return-type` / `ex-exists-param-type` fixtures shipped via cross-plan-followups F1-1.  `exg4-pack-into-struct` remains blocked on `defstruct`'s field-type parser only accepting simple keyword/sym types -- track that as a separate `defstruct` extension. |
| EXG5-1 | EXG5  | Kind tag in `RcControlBlock::reserved` -- **shipped** (`reserved[0]` = `RCK_*`, `reserved[1]` = `RCEXP_*`; mirrored in `emit_module.c` so generated code sees identical layout) |
| EXG5-2 | EXG5  | Cycle-walker dispatch on the kind tag -- **shipped** (`gc_mark_phase` propagates from strong roots through `RCK_EXISTENTIAL` blocks whose `RCEXP_RC` payload points at another `RcControlBlock`; same logic in the inline runtime) |
| EXG5-3 | EXG5  | Pack-site writes payload descriptor -- **shipped** (`emit_expr.c` `EX_EXISTS_PACK` uses `rc_cb_alloc_kinded`; payload kind is `RCEXP_RC` when the packed value's type is `TY_RC`, `TY_WEAK`, or a constrained `TY_EXISTS`, else `RCEXP_OPAQUE`) |
| EXG5-4 | EXG5  | `rc_cb_alloc_kinded` to honour `may_contain_cycles` -- **shipped** (variant added in `rc.{h,c}`; `rc_cb_alloc` now forwards to it with `RCK_OPAQUE`).  The `may_contain_cycles=false` short-circuit for primitives is unchanged: the existing `value_type_kind<=7` rule already covers all scalar payloads |
| EXG5-5 | EXG5  | Cycle-collection tests for existential payloads -- **mostly shipped:** original wave (`exg5-kind-tag-opaque`, `exg5-kind-tag-rc`, `exg5-walker-rc-payload`) plus `exg5-rc-in-exists` via cross-plan-followups F1-2 (move-at-pack + smart drop hook).  The full cycle-construction `exg5-exists-cycle` is deferred -- it needs a back-edge from an rc<Cell>-typed struct field to the existential containing it, which requires the same `defstruct` compound-annotation extension that blocks `exg4-pack-into-struct`. |
| EXG6-1 | EXG6  | `:linear` attribute on `exists` -- **shipped** (parsed in `elab_types.c`; flag stored on `forall_.is_linear`; the type's `copy_kind` is upgraded to `CK_LINEAR` so let-binding sites pick it up via the existing `^linear`/CK_LINEAR path) |
| EXG6-2 | EXG6  | Substructural use-exactly-once check -- **shipped** (reuses LT1: the let binding inherits `is_linear`; `open` consumes via the existing EX_VAR lookup; zero opens trip `TUR_E0100`, two opens trip `TUR_E0101`).  Requires `-Xlinear` |
| EXG6-3 | EXG6  | Linear emit path (no RC header) -- **shipped** (linear pack uses plain `malloc(sizeof(tur_existential_t) + ...)`; `EX_EXISTS_OPEN` reads the record directly and emits `free((void *)(packed))` at the end of its body) |
| EXG6-4 | EXG6  | (Confirm no EXG4/EXG5 interaction) -- **shipped** by construction: the EXG4 let-tail-move scan only fires for `CK_MOVE` bindings, linear existentials are `CK_LINEAR`; EXG5's walker only follows `RCK_EXISTENTIAL` blocks allocated via `rc_cb_alloc_kinded`, linear packs go through plain `malloc` and are never registered with the GC.  Also: the EXG1-5 auto-drop is skipped for linear existentials so the per-open `free()` is the sole release |
| EXG6-5 | EXG6  | Linear-existential tests -- **shipped:** `exg6-linear-ok`, `exg6-linear-multi`, `errors/exg6-linear-unused`, `errors/exg6-linear-double-open` from the original wave plus `errors/exg6-linear-escape` via cross-plan-followups F2-1 (defn-boundary diagnostic on `:linear` return types). |

---

## Relation to Other Plans

- **`existential-gc-plan.md`** (shipped EXG1; partial EXG2 inherited
  from `rc_cb_alloc`).  This plan picks up EXG2-2/EXG2-4 (folded into
  EXG5) and EXG3 (folded into EXG6), plus the cross-scope-ownership
  gap explicitly called out in EXG1-5's followup notes.
- **`existential-types-plan.md`** (EX1--EX2).  EXG4-3 was retired
  (see status banner); the cross-plan followups F1-3 reasoning
  applies.
- **`refinement-types-plan.md`** -- independent.  Refinements narrow
  the hidden type's range at the open boundary but do not change the
  storage discipline.
- **`typed-collections-plan.md`** -- storing a packed constrained
  existential in `Vec[Showable]` requires an explicit `rc/clone` at
  the insert site (matching the `rc<T>` discipline); without the
  clone, the originating binding's drop and the vec's drop both fire
  against the same control block.  EXG4-3 was retired (see status
  banner).
