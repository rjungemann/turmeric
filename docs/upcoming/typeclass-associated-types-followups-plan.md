---
title: Typeclass Associated Types -- Follow-up Plan (multi-param classes, fundeps, value-level projection)
category: Planning
description: The two pieces left out of the minimal associated-types milestone -- multi-parameter typeclasses with functional dependencies, and value-level projection through an associated type (a method whose ABI depends on the projected type). Builds directly on the shipped single-parameter, type-level-only associated types.
---

# Typeclass Associated Types -- Follow-up Plan

> **Status:** Not started. The single-parameter, type-level associated-types
> milestone has shipped (see "Background" below); this plan covers the two
> pieces explicitly left out of scope.
>
> **Prerequisites:** the associated-types milestone
> (`docs/reported/typeclass-associated-types-missing.md`, resolved) and the
> existing per-type-parameter constraint infrastructure (`TypeConstraint`,
> `param_idx`, `tyvar`; Phase PTC1--PTC6, Phase RT).
>
> **Flag:** `-Xassoc-types-2`. Both parts are gated behind one flag so the work
> can land incrementally without disturbing existing dispatch. The flag implies
> nothing else; the shipped (unflagged) single-param associated types stay on.
>
> **Last updated:** 2026-06-11

---

## Motivation

The associated-types milestone deliberately shipped the narrow, low-risk core:

- **One type parameter per class.** `(Elem (Vec int))` resolves because the
  class has exactly one type argument to match on.
- **Type-level only.** A projection `(Name T)` is usable in any *type
  annotation*; it is erased before codegen. No method's C signature can depend
  on `(Name T)`.

Two real use cases in flight want more:

1. **`Component -> Storage` (ECS, `docs/upcoming/v1/ecs-spice-plan.md`).** The
   ECS plan wants `(definstance Component Pos (type Storage = (DenseStorage Pos)))`
   *and* a method `(make-storage [] : Storage)` / `(iter-storage [s : Storage] ...)`
   whose argument and result types are the projected `Storage`. That is
   **value-level projection** (Part B). Today the storage handle rides the
   int64 carrier untyped, so a `get-Vel` accidentally handed a `Pos` storage is
   not caught.

2. **`Collection k v`, `Convert a b`, coercion/conversion classes.** Anything
   shaped "two related type parameters, one functionally determines the other"
   wants a **multi-parameter class with a functional dependency** (Part A):
   `(defclass Collect [c e] | (c -> e) ...)` so `e` is pinned by `c` and the
   projection `(Elem c)` can be a multi-parameter lookup.

Non-goals (still out, even after this plan):

- Type families with arithmetic / equations beyond a single output type.
- Overlapping or incoherent instances; the orphan + idempotence rules carry
  over unchanged.
- Higher-rank associated types (an associated type that is itself `forall`-d).

---

## Background: what shipped

The single-parameter, type-level milestone (citations are current `main`):

| Piece | Location |
|---|---|
| `TypeClass.assoc_type_names` / `n_assoc_types` | `src/compiler/typeclass.h:53` |
| `TypeClassInstance.assoc_types` / `n_assoc_types` | `src/compiler/typeclass.h:96` |
| `typeclass_env_find_assoc_type` (name -> class + index) | `src/compiler/typeclass.c:75` |
| `typeclass_env_resolve_assoc_type` (precise `type_eq` match) | `src/compiler/typeclass.c:93` |
| `defclass` member parse `(type Name : Type)` | `src/compiler/elab_typeclasses.c:1092` |
| `definstance` binding parse `(type Name = T)` + resolve | `src/compiler/elab_typeclasses.c:2001` |
| Projection hook (generic annotations) | `src/compiler/elab_types.c:1486` |
| Projection hook (defn signatures, `fn_type_from_form`) | `src/compiler/elab_fns.c:108` |
| Fixture | `tests/fixtures/assoc-type-projection/` |

Key facts this plan builds on:

- **Resolution is precise**, not kind-erased: `typeclass_env_resolve_assoc_type`
  walks instances and matches the *single* `type_args[0]` with `type_eq`. This
  is the function Part A must generalise to N arguments.
- **The projection hook lives in two parsers**, because `fn_type_from_form`
  (`elab_fns.c`) builds type applications itself and never reaches
  `type_expr_from_form`. Any new resolver entry point must be wired into both.
- **Associated types are erased.** Part B's whole job is to *stop* erasing them
  at method-ABI boundaries -- carefully, since the dictionary lowering
  (`docs/guides/typeclass-internals-guide.md`) assumes a fixed C signature per
  method.

Existing constraint infrastructure Part A reuses rather than reinvents:

| Piece | Location | Reuse |
|---|---|---|
| `TypeConstraint{ typeclass, type_arg, param_idx, tyvar, return_resolved }` | `src/compiler/typeclass.h:175` | Per-arg constraints already index into TY_APP element types via `param_idx`. |
| `typeclass_instance_constraints_satisfied` | `src/compiler/typeclass.c` | Already substitutes `param_idx` from concrete element types -- the substrate for fundep propagation. |
| Return-only dispatch (`method_is_return_dispatch`, Phase RT) | `src/compiler/elab_typeclasses.c` | A method whose dispatch variable appears only in the return type already selects its instance from the expected type; fundeps generalise this selection. |

---

## Part A -- Multi-parameter classes & functional dependencies

### Surface syntax

```turmeric
;; Two parameters; `c` functionally determines `e` (the fundep `c -> e`).
(defclass Collect [c e] | (c -> e)
  (empty   [] : c)
  (insert  [self : c x : e] : c)
  (to-list [self : c] : int))

(definstance Collect [(Vec int) int]
  (empty       (vec-new))
  (insert  [self x] (vec-push self x))
  (to-list [self]   (vec-len self)))
```

The `| (c -> e)` clause (pipe + arrow list) is the functional dependency: for a
given `c` there is at most one `e`. A class with no `|` clause and >1 parameter
is still allowed but every parameter must be fixed at the dispatch site (no
inference of the unconstrained ones).

Associated types compose: a single-output associated type is the degenerate
fundep `c -> Elem`. Part A makes the *general* lookup work; Part A keeps the
shipped single-param associated-type sugar as-is.

### Phases

**MP0 -- N-ary instance heads.** `definstance` already parses a vector of type
args and combines adjacent symbols into `TY_APP` (`elab_typeclasses.c`, the
HKT-P1 combining loop). Generalise the arity check and the codegen suffix
(`build_inst_type_suffix`) so a 2+ parameter instance head produces a distinct,
collision-free dictionary name. Pure plumbing; no dispatch change yet. Gate the
">1 type parameter" path behind `-Xassoc-types-2`.

**MP1 -- N-ary precise lookup.** Add `typeclass_env_lookup_instance_exact(env,
tc, type_args, n)` that matches **all** `n` arguments by `type_eq` (the
existing `typeclass_env_lookup_instance` is kind-erased and first-match; do not
touch it -- value-method dispatch depends on its current behaviour). This is
`typeclass_env_resolve_assoc_type`'s matcher lifted to N args.

**MP2 -- Fundep storage + parse.** Store the fundep on `TypeClass`:
`uint8_t fundep_from_mask; uint8_t fundep_to_mask;` (bitsets over type-param
indices; one fundep in v1). Parse `| (a b -> c)` in `defclass` after the
type-param vector. Validate: every name appears in the param list; `to` and
`from` are disjoint.

**MP3 -- Fundep-driven inference.** At a method call where the determining
parameters (`from`) are fixed by the arguments, resolve the instance by
`from`-only match and **propagate** the determined `to` parameters from the
instance head, instead of requiring the caller to fix them. This reuses the
return-dispatch machinery (Phase RT): the determined parameter is treated like
a return-only dispatch variable, selected from the matched instance. Emit
`TUR-E00xx` "functional dependency violated" when two instances share a `from`
projection but disagree on `to` (coherence check, MP5).

**MP4 -- Multi-param associated-type projection.** Generalise
`typeclass_env_resolve_assoc_type` and both parser hooks
(`elab_types.c:1486`, `elab_fns.c:108`) to accept `(Name T1 T2 ...)` for an
N-parameter class, using MP1's exact lookup. The single-arg path stays the fast
default.

**MP5 -- Coherence / overlap check at `definstance`.** When registering a
multi-param instance, reject a new instance whose `from`-projection collides
with an existing one under the declared fundep (the existing idempotent
re-instance guard at `elab_typeclasses.c` is the natural home -- extend its key
from "type-arg suffix" to "fundep-from suffix"). This is the multi-param analog
of the single-param "instance already defined" rule.

### Part A validation

- `(defclass Collect [c e] | (c -> e) ...)` with `Collect [(Vec int) int]` and
  `Collect [(Vec cstr) cstr]`: `(insert v 7)` on a `Vec int` infers `e = int`
  without the caller writing it; `(insert v "x")` is a type error.
- A second instance `Collect [(Vec int) cstr]` is rejected at the
  `definstance` with "functional dependency violated".
- `(Elem (Map cstr int))` (2-arg projection) resolves via MP4.

---

## Part B -- Value-level associated-type projection

### Goal

Let a method's parameter or result type *be* an associated type, so the
projected type flows into the method's C signature and is checked at call
sites:

```turmeric
(defclass Component [c]
  (type Storage : Type)
  (make-storage [] : (Storage c))               ;; result is the projection
  (iter-storage [s : (Storage c)] : int))       ;; param is the projection

(definstance Component [Pos]
  (type Storage = (DenseStorage Pos))
  (make-storage     (dense-storage-new))
  (iter-storage [s] (dense-len s)))

;; A Vel storage handed to Pos's iterator is now a type error, not a silent
;; int64 mismatch.
```

### The core difficulty

The dictionary-lowering guide is explicit: *the dict-field type, the
impl-signature type, and the call-site cast type must all agree.* Today
`(Storage c)` is erased to the int64 carrier at all three, which is exactly why
it "works" but is unchecked. Part B must instead resolve `(Storage c)` to the
instance's bound `assoc_types[k]` **at the point each of those three C types is
computed**, per instance. The bound type may be:

- a primitive / struct / `TY_APP` with a real C name (lower to that name), or
- an opaque handle (lower to int64, as today, but *typed* in the elaborator so
  the cross-instance checks fire).

### Phases

**VP0 -- Resolve projections in method signatures against the instance.** When
elaborating a `definstance` method (`elab_typeclasses.c`, the per-method
pass-1/pass-2 loop), substitute any `(AssocName <class-var>)` in the method's
declared param/return types with *this instance's* `assoc_types[k]`. The class
already substitutes a bare class tyvar return type with the instance type arg
(Phase RT, `return_type.kind == TY_TYVAR` block); add an analogous substitution
that recognises an associated-type projection over the class variable and
swaps in `inst->assoc_types[k]`. Output: the method's elaborated `Type` carries
the real projected type, per instance.

**VP1 -- Thread the projected type into the three C sites.** Ensure the
substituted type reaches:
1. the dict struct field type (`emit_stmt.c`, `EX_INSTANCE_DEF`),
2. the impl signature (`emit_fns.c`), and
3. the call-site cast (`emit_expr.c`, `EX_DICT` dispatch).
This is the same "three views of one function pointer" invariant from the
internals guide; the projected type must be applied identically at all three.
For a projected type that is an opaque/`TY_APP` handle this is still int64 --
the change is that the *elaborator* now knows the type, even where C stays
int64.

**VP2 -- Check projections at use sites.** A call `(.iter-storage d s)` where
`s`'s static type is not `type_eq` to `(Storage <receiver-type>)` is a
`TUR-E0001`. Because VP0 gives each instance a concrete method type, this falls
out of the normal argument-type check once the dispatcher selects the instance
-- the work is making the dispatcher resolve the projection for the *selected*
instance before checking arguments, rather than against the erased class
signature.

**VP3 -- Projection in the method body.** Inside an instance method, a local
typed `(let [x : (Storage c) ...])` or an intra-instance `(.sibling self ...)`
returning `(Storage c)` must resolve against the enclosing instance. Reuse VP0's
substitution with the in-scope instance, mirroring the existing intra-instance
dispatch (`docs/archive/history/intra-instance-method-dispatch-unsupported.md`).

**VP4 -- Default methods.** A `defclass` default body (`__default_<Class>_<m>`,
`elab_typeclasses.c` third pass) cannot resolve `(Storage c)` -- there is no
instance. Default bodies that mention an associated type must stay polymorphic
(carry the projection as an unresolved marker) and only get a concrete type
when specialised per instance, or be rejected with a clear diagnostic in v1.
Decision point -- see Open Questions.

### Part B validation

- `tests/fixtures/assoc-type-value-projection/`: a `Component` class with a
  `Storage` associated type, two instances binding distinct storage structs,
  and a method `iter-storage [s : (Storage c)]`. Passing the wrong instance's
  storage is a compile error reported at the call site; passing the right one
  type-checks and runs.
- The ECS plan's `defcomponent` macro lowers to exactly this and drops its
  parallel macro-registry workaround (cross-link
  `docs/upcoming/v1/ecs-spice-plan.md`).

---

## Coherence & invariants (carry-over)

- **Orphan rule** (`elab_typeclasses.c` orphan check) is unchanged: a
  multi-param instance is non-orphan if the class or *any* type argument is
  local. Associated-type bindings do not create new orphan obligations.
- **Idempotence**: the re-instance guard generalises in MP5; a byte-identical
  re-`definstance` (including identical associated-type bindings) stays a silent
  no-op for reload-safety.
- **Erasure boundary**: Part A is fully erased (type level). Part B is the only
  place an associated type touches codegen, and only through the existing
  three-site function-pointer invariant.

## Open questions

1. **VP4 default bodies.** Reject associated-type mentions in default methods in
   v1 (simplest, matches "no proof terms"), or carry an unresolved projection
   marker and specialise per instance (more work, needed if any stdlib class
   wants a defaulted projection method)? Recommendation: reject in v1 with a
   targeted diagnostic; revisit if a concrete need appears.
2. **Multiple fundeps per class.** v1 stores one `from -> to`. Is a second
   fundep ever needed before v1 ships? Recommendation: single fundep; widen the
   masks to an array only on demand.
3. **Fundep vs. associated type overlap.** A single-output associated type *is*
   a fundep `c -> Elem`. Should `(type Elem : Type)` implicitly add the fundep
   so the determined-parameter inference (MP3) also drives `Elem`? Recommendation:
   yes -- treat each associated type as an implicit fundep target; this unifies
   the two features and is why they share one plan and one flag.

## Sequencing

Part A (MP0--MP5) and Part B (VP0--VP4) are independent and can land in either
order behind the shared flag. Part B is the higher-value, higher-risk half
(it is the only codegen-touching work and unblocks ECS); Part A is lower-risk
plumbing over existing constraint infrastructure. Suggested order: **MP0--MP1
first** (they give Part B its N-ary exact matcher for free), then **VP0--VP2**
(the ECS-unblocking core), then the remainder as demand warrants.

## Related

- `docs/reported/typeclass-associated-types-missing.md` -- the shipped milestone.
- `docs/guides/typeclass-internals-guide.md` -- dictionary lowering + the
  three-site function-pointer invariant Part B must respect.
- `docs/upcoming/v1/ecs-spice-plan.md` -- the primary consumer of Part B.
