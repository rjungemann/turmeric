---
title: EF-3 -- Decide + (maybe) lower `(forall [a] T)` as a struct/ADT field
category: Planning
description: First a design decision (is a rank-N field meaningful?), then, if yes, route + carrier + fixture. Umbrella at structdef-exotic-field-forms-plan.md.
---

# EF-3: `forall` as a struct/ADT field

Predecessor: [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md).
Sequenced after [EF-2](structdef-exotic-field-handler-plan.md); shares
the "add a storage case" pattern but is gated by a design decision.

## Why this needs a design phase

The DS-A4 gate comment historically flagged `forall` as "not a
value-carrying field form." Rank-N polymorphism in a field means storing
a value whose *type* is `forall a. T[a]` -- concretely a poly-wrapper.
Whether Turmeric wants first-class poly-fields at v1 is not settled. This
plan starts with that decision, then either lowers or moves the form to
a permanent "unsupported" note.

## Phase 1 -- Decide

- **P1.T1.** Write up (in this doc, appended) what a `forall` field
  would mean semantically:
  - Does it store a poly-wrapper (closure over a type parameter)?
  - Or is it always instantiated at struct-construction time (and thus
    equivalent to the concrete type)?
  - How does it interact with monomorphization (the end-to-end
    monomorphization north-star)?
- **P1.T2.** Survey producers: grep the stdlib, spices, and fixture tree
  for any user that *wants* a `forall`-typed field but has to work
  around it. If none, weigh whether to ship the feature ahead of a
  concrete demand.
- **P1.T3.** Decide: **lower**, **permanent-reject**, or **shelve**.

Phases 2-4 are conditional on `lower`. If `permanent-reject`, jump to
Phase 5.

## Phase 2 -- Route the head (conditional: lower)

- **P2.T1.** Drop `e->sym_forall` and `e->sym_forall_u` from the DS-A4
  rejection block.
- **P2.T2.** Add a dispatch to `type_expr_from_form` (mirrors `exists`
  at line 226-228 -- `exists`-pack fields already lower via the same
  path).

## Phase 3 -- Storage (conditional: lower)

- **P3.T1.** `struct_field_storage_from_type` already maps `TY_FORALL`
  to `TY_INT` (line 158). Verify no additional whitelist entry is
  needed on the record-ADT field-lowering path.
- **P3.T2.** Confirm the value representation: a `forall`-typed field
  needs a runtime carrier that survives instantiation. Cross-check with
  the end-to-end monomorphization plan -- a poly-field is one of the
  cases that plan explicitly reasons about.

## Phase 4 -- Fixture + verify (conditional: lower)

- **P4.T1.** `tests/fixtures/defstruct-field-forall/` -- construct a
  struct holding a poly-value, instantiate at two different types,
  print both.
- **P4.T2.** Regenerate snapshots, full-suite run.

## Phase 5 -- Permanent-reject path (conditional)

- **P5.T1.** Move the `forall` bullet out of the umbrella
  `structdef-exotic-field-forms-plan.md` into a "permanently
  unsupported field forms" note (create if none exists).
- **P5.T2.** Keep the rejection in `struct_field_type_from_form` but
  reword the diagnostic to reference the new note instead of the
  umbrella plan.
- **P5.T3.** Delete this plan file (or move to `docs/archive/`).

## Phase 6 -- Umbrella update

- **P6.T1.** In either outcome, remove `forall` from the umbrella
  status table and update its "rejected forms" list.

---

## Phase 1 outcome (2026-07-02): SHELVE

Decision: **shelve**. Keep the DS-A4 rejection in place; do not lower and
do not declare permanently unsupported. Revisit when (a) a concrete
producer wants a poly-typed field and (b) the HRT consumption path can
instantiate a poly value read out of a field. Neither holds today.

### P1.T1 -- what a `forall` field would mean semantically

A field of type `(forall [a] T[a])` stores a **poly-value**: a value that
can be instantiated at any `a` when read back out. It is the dual of the
`exists`-pack field that already lowers (`exg4-pack-into-struct`):

- `exists` field: pack a *concrete* value under a hidden type; the
  consumer `open`s it to recover an abstract-but-usable binding. The
  consumption path (`open`) is wired.
- `forall` field: store a *genuinely polymorphic* value; the consumer must
  **instantiate** it at a chosen type before use. The consumption path
  (instantiate-from-field) is **not** wired.

Storage is the easy half and is already free -- it rides the exact same
carrier as `exists`:

- `struct_field_storage_from_type` (`src/compiler/elab_structs.c:158`)
  already maps `TY_FORALL` -> `TY_INT` (int64 carrier), grouped with
  `TY_TYVAR`/`TY_EXISTS`/`TY_STRUCT`/`TY_ADT`.
- The C emitter renders `TY_FORALL` as `void *` (`types.c`), and
  `emit_expr.c` handles `TY_FORALL` uniformly alongside `TY_EXISTS`
  (box as `(int64_t)(intptr_t)`, rc-bridge, etc.).
- Routing is a one-line change: add `sym_forall`/`sym_forall_u` to the
  `exists` dispatch in `struct_field_type_from_form` and drop them from
  the DS-A4 rejection block. Verified: with that change a
  `(defstruct Cap [h (forall [a] a)])` `emit-c`s cleanly.

**Interaction with monomorphization / HRT.** Turmeric's rank-N support
(HRT) is *erasure-based*: a poly-fn parameter is carried as a wrapper
pointer, and the only values accepted at a rank-N argument position are
**named functions** (`hrt-impred-*`, `hrt-rankn-*` fixtures all pass a
bare `defn` name, ascribed via `(:: f (forall [a] (-> a a)))`). There is
no facility to instantiate an arbitrary *expression* of forall type -- and
a field read `(.f b)` is an expression, not a named function. So a stored
poly-value is genuinely **write-only**:

  ```turmeric
  (defstruct PolyBox [f (forall [a] (-> a a))])
  (defn id [x : int] : int x)
  ;; store: OK
  (make-struct PolyBox (:: id (forall [a] (-> a a))))
  ;; consume, attempt 1 -- direct call:
  ((.f b) x)          ; error: type `(forall [a] (fn [int] : int))` is not callable
  ;; consume, attempt 2 -- pass to a rank-N param:
  (apply-poly (.f b) x) ; error: rank-2 argument must be a named function
                        ;        (capturing closures not yet supported)
  ```

Both consumption paths are dead ends with the current HRT machinery.
Wiring field-read -> poly-instantiation is real work (it is the same gap
as "capturing closures as rank-N args"), well beyond the "add a storage
case" recipe the sibling EF plans follow. This is the crux: the plan's
P4.T1 fixture ("instantiate at two different types, print both") **cannot
be written** today.

### P1.T2 -- producer survey: NONE

No `defstruct`/`defdata` in stdlib, fixtures, examples, benchmarks, or the
(absent) `../turmeric-spices/` checkout uses a `(forall ...)` field. Every
`(forall ...)` in `.tur` sources is a function parameter/return type
(rank-N args) or a `deftype` alias. The only `forall`-as-field declaration
in the tree is the **negative** fixture
`tests/fixtures/errors/defstruct-exotic-compound-field-rejected` that
asserts it is rejected. There is no latent demand to serve.

### P1.T3 -- decision rationale

- **Not `lower`:** lowering storage alone ships an API surface the type
  checker accepts but no one can consume -- exactly the downstream footgun
  CLAUDE.md warns against -- and there is zero demand for it. The
  advertised end-to-end fixture is unachievable until HRT can instantiate
  a poly value from a non-named-function position.
- **Not `permanent-reject`:** there is no fundamental reason `forall` can
  never be a field. It is blocked by an *in-flight* HRT limitation
  (erasure-based rank-N, named-function-only consumption), not by a
  permanent semantic conflict. Declaring it permanently unsupported would
  be inaccurate and would have to be walked back once HRT matures.
- **`shelve`:** keep the honest "not yet supported ... tracked in
  <plan>" diagnostic (DS-A4 block, unchanged). Storage is ready; the gate
  is the consumption path plus a real producer. When both arrive, the
  route change is one line and Phases 2-4 apply as written.

Re-entry criteria (what unshelves this): a concrete field-holding-a-poly
use case appears, **and** HRT gains instantiation of a forall value read
from a field (or the language moves to a boxed poly-wrapper that survives
instantiation). At that point drop the two `sym_forall*` symbols from the
DS-A4 block, add them to the `exists` dispatch, and write the fixture.
