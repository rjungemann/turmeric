---
title: Variadic HKT rows (kind-level type lists) are missing from the elaborator
category: Expressiveness hole
severity: Forces an arity-N macro-family workaround for any "typed heterogeneous row" feature (ECS queries, relational rows, data-frame schemas). Not blocking for v1 work that accepts the cap.
description: The HKT machinery supports fixed-arity type constructors but has no kind-level list-of-types, so a `Query in out` parameterised by a row of components, a relation parameterised by a tuple of columns, or any other variadic-row abstraction has to be macro-generated as `Query1`..`QueryN` -- the same retreat Bevy/Specs/aztecs took. The downstream plans (ECS v1) have absorbed this by capping at arity 5 with a documented migration path.
---

# Variadic HKT rows are missing

## Summary

Turmeric's HKT support (per `docs/guides/hkt-guide.md`) lets a type
constructor be abstracted over and partially applied at the type level,
but **the kind system has no `List Type` (or `Row :: List Type`)**. Every
HKT-using abstraction today is parameterised by a fixed-arity tuple of
type arguments. There is no way to write `Query (cons Pos (cons Vel
nil)) (cons Pos nil)` and have the elaborator see the row structurally.

Workarounds in practice are macro-generated families
(`Query1`/`Query2`/.../`QueryN`), capped at some arity N. This is the
pattern Specs / Bevy / aztecs all settled on in Haskell/Rust; the whole
pitch of the ECS plan (and of `stats-formula-plan`, and any future
relational layer) is that we would not have to.

## Where this bites

- `docs/upcoming/v1/ecs-spice-plan.md` ships `queryN` (arity 1..5) on
  the v1 track and reserves the variadic `query` form for v2 E1, gated
  on this work. The arity cap is the same retreat Bevy/Specs took; we
  accept it for v1 with a documented migration path, but the original
  pitch ("more invariants at the type level than Haskell") is weaker
  until it lifts.
- `stats-formula-plan` and any future data-frame work want the same
  primitive (rows of columns) and would have to re-implement the same
  macro family, then re-migrate when this lands.
- A general relational / Datalog-results layer over heterogeneous tuples
  is currently expressible only via untyped cons-lists with downcasts at
  the use site, or via the same arity-N macro pattern.

## Observed vs. expected

**Observed.** No `Row` / `List Type` kind. No way to write a type
parameter that ranges over "a tuple of N types for any N." The HKT plan
in the archive treats associated types and type-level lists as
"deferred / not started."

**Expected (for the plans that depend on this).** A kind-level
`List Type` (or a dedicated `Row` kind), a `Row` HKT, and the elaborator
machinery to:

- check membership (`Pos in row`) against a row term,
- form unions/intersections of rows for query joins,
- propagate row arguments through partial application of a higher-kinded
  constructor.

## Root cause

The elaborator's kind language is fixed: `Type`, `Type -> Type`,
`Type -> Type -> Type`, etc. Adding a `List Type` kind is a real
elaborator project (new kind constructor, unification rules for cons /
nil at kind level, normalisation, fixture work). It is not a small
patch.

## Proposed directions

1. **Fund the elaborator work.** Add a `Row` (or `List Type`) kind,
   row-level cons/nil, structural row equality, and `in`/`++`
   primitives. Validate against `tests/fixtures/hkt-row-*` and an ECS
   query fixture.
2. **Document the macro-arity fallback as the v1 reality.** If we are
   not funding (1) in this release window, update `hkt-guide.md`,
   `ecs-spice-plan.md`, and `stats-formula-plan` to say so explicitly,
   so downstream plans stop quoting variadic rows as available.
3. **Hybrid: ship `Row` as runtime-only first.** A list-of-`TypeRep`
   row would unblock query macros at the cost of giving up the
   compile-time membership check. This is what Haskell ECSes do; we
   would be no worse off than they are.

## Validation of a fix

- A query / row fixture that declares `Query [Pos Vel] [Pos]` and
  `Query [Pos] [Pos]` and gets a compile error from
  `Query [Pos Vel] [Foo]` when `Foo` is unregistered.
- Row-polymorphic identity: `(defn id-row [r] (fn [x : Row r] x))`
  type-checks and is callable at distinct row instantiations.
- Stats / data-frame fixture: `Frame [Name :str Age :int]` zip with
  `Frame [Age :int Name :str]` is a row-permutation no-op, not a type
  error.

## Progress (Direction 1 -- funding the elaborator work)

Direction 1 is the chosen path. It is a multi-layer elaborator project; the
layers are being landed incrementally, each validated on its own before the
next builds on it, so no half-finished layer can silently miscompile.

### Layer 1 -- kind-level `List Type` in the kind language [LANDED]

The kind language was fixed at `KIND_STAR` and the `KIND_ARROW{N}` ladder
(plus the `KIND_ROW` *effect*-row sentinel, an unrelated concept). The first
layer adds a distinct kind-level list-of-types sentinel so the rest of the
machinery has a kind to attach a row to:

- `KIND_TYPEROW ((Kind)0xFFFE)` in `src/compiler/types.h` -- the kind of a
  *row of types* (`[Pos Vel]`). Disjoint from `KIND_ROW` (0xFFFF, effect rows)
  and from every arrow arity.
- `kind_to_string` / `kind_parse` round-trip it as `"[*]"`
  (`src/compiler/types.c`).
- `kind_apply_one` treats it as inert (identity), like `KIND_ROW`: a row is a
  first-class kind, not a constructor you apply.
- `kind_is_arrow_ladder` (`src/passes/kind_check.c`) excludes it, so
  `kind_unify` rejects `[*]` vs `*` / `* -> *` / `Row` with `TUR-E0012`, and
  `kind_of_type_app` rejects applying a row as a constructor.
- Unit target `tur_kind_row_unit` (`tests/compiler/test_kind_row.c`, registered
  in `src/CMakeLists.txt` + root `CMakeLists.txt`) pins the parse/print
  round-trip, sentinel disjointness, the apply/unify algebra, and the
  diagnostic counts for the mismatch cases.

### Prerequisites surfaced by a code-level pass (do these first)

A detailed read of the type/parser/codegen subsystems turned up three
prerequisites that were *not* in the original layer list and that change the
sequencing. Tackle them before the type-variant work.

- **PRE-1 [RESOLVED -> `#row{...}` reader form]: the bare-`[...]` row syntax in
  the report's original examples is already taken.** Since KB-029, a bracketed
  type list `[T1 T2 ... Tn]` in *type-annotation position* lowers to the stdlib
  `TupleN` struct (arity 2..8) -- see `elab_types.c:1519-1553` (`F_VEC` branch
  of `type_expr_from_form`). So `Query [Pos Vel] [Pos]` as written would
  silently become `Query (Tuple2 Pos Vel) (Tuple1 Pos)`, i.e. two `KIND_STAR`
  tuples, not rows. Bare brackets are therefore unusable for rows.

  **Decision: spell rows with a `#row{...}` reader-dispatch form**, alongside
  the existing `#map{...}` / `#set{...}` data literals (`reader.c:1035`,
  `try_read_data_literal`). Rationale:
    - It is a *distinct* form, so it never perturbs the `[...]`->`TupleN`
      tuple-sugar path and needs no context-driven disambiguation (PRE-2 is
      consequently **dropped** -- `type_expr_from_form` stays context-free).
    - It reuses an existing, tested reader-dispatch mechanism (~10 lines added
      to the `try_read_data_literal` table for `#row{`).
    - It scales to *labeled* rows for the data-frame case for free, by mirroring
      `#map{...}`'s keyword-key/value reader: `#row{Pos Vel}` is positional
      (ECS), `#row{:name :str :age :int}` is labeled (frames).
    - Spiritually identical to Rust `frunk`'s `hlist![A, B]` macro and Haskell
      DataKinds' promoted list `'[A, B]`, in Turmeric's house style.

  Surface examples (revised; supersede the bracket forms in the validation
  section):

  ```turmeric
  (defn run [q : (Query #row{Pos Vel} #row{Pos})] : int ...)  ; positional, ECS
  (def schema : #row{:name :str :age :int})                    ; labeled, frame
  ```

  **Rejected alternatives:**
    - *Haskell-style tick* `'[Pos Vel]` -- `'` is already the reader macro for
      `(quote x)` *and* for lifetime symbols `'a` (`reader.c:162,511`), so a
      leading tick on a bracket reads as `(quote [...])`. Unavailable.
    - *Context-driven bare `[...]`* (old option 2) -- would require threading an
      expected kind through `type_expr_from_form` (PRE-2) and risks perturbing
      the tuple-sugar path. Heavier and riskier than a distinct reader form.
    - *Unify tuple and row* (old option 3) -- treat `TupleN` as the `KIND_STAR`
      view of a row. Elegant but couples two features; deferred, not blocking.

- **PRE-2 [DROPPED by the PRE-1 resolution]: thread an expected kind into type
  elaboration.** This was only needed for the context-driven bare-`[...]`
  option. With the `#row{...}` reader form, `type_expr_from_form`
  (`elab_types.c:271`) stays context-free -- a `#row{...}` literal carries its
  own dispatch tag, so no expected-kind threading is required. Recorded here
  for history; not on the critical path.

- **PRE-3 (safety, blocking Layer 2): audit the `TypeKind` switches before
  adding a variant.** A new `TY_TYPEROW` is a miscompile trap until every
  switch handles it. The dangerous ones found:
    - `type_eq` (`types.c:51`) ends in **`default: return 1`** -- two distinct
      `TY_TYPEROW`s would compare **equal** (silent miscompile). MUST add an
      explicit structural case (mirror the `TY_UNION`/`TY_INTERSECTION` member
      loop at `types.c:179-201`).
    - `type_name_buf` (`types.c:1503`) has **no default** -- a missing case
      prints nothing.
    - `type_c_name` (`types.c:1893`) falls back to `"void"`; add an explicit
      erase case (rows are compile-time only, like `TY_TYPECLASS`/`TY_GLOBAL`
      which already erase to `void`/`void*`).
    - `type_effective_kind` (`kind_check.c:127`) defaults to `KIND_STAR`; add a
      case returning `KIND_TYPEROW`.
    - the `copy_kind` switch (`types.h:~300-400`) -- rows are compile-time, so
      `CK_COPY`.
  The `Type` union should mirror the `TY_UNION` representation exactly:
  `struct { struct Type **elements; uint8_t n_elements; } typerow_;`
  with an arena-allocated `Type*` array (`type_union_build` at `types.c:2712`
  is the constructor to copy).

### Layer 2 -- `TY_TYPEROW` type variant [LANDED]

The compile-time row-of-types value, built and validated entirely at the C
level (no parser/codegen dependency yet). PRE-3 was executed alongside it.

- `TY_TYPEROW` added to the `TypeKind` enum (`types.h`); union member
  `typerow_ { Type **elements; uint8_t n_elements; }`, mirroring `union_` /
  `intersection_`. `type_typerow(arena, elements, n)` constructor copies the
  element-pointer array onto the arena, sets `hkt_kind = KIND_TYPEROW` and
  `copy_kind = CK_COPY`. Element order is significant, duplicates are
  preserved, and nested rows are *not* flattened (a row is a list, not a set).
- **Equality.** `type_eq` gains an explicit order-SENSITIVE structural case
  (the PRE-3 landmine: without it, two rows fell through `default: return 1`
  and compared equal). `type_typerow_eq_perm` is a separate order-INSENSITIVE
  (multiset) comparator backing the data-frame column-permutation case.
- **PRE-3 switch audit, enforced by `-Werror=switch`.** Handled in `type_eq`,
  `type_name`, `type_name_buf` (prints `#row{...}`), `type_c_name` (erases to
  `/*type-row*/ void`), `typekind_default_copy_kind` (`CK_COPY`),
  `type_is_guarded_recursive_helper` (guards like a union), and
  `type_effective_kind` in `kind_check.c` (`KIND_TYPEROW`). A full
  `-Werror=switch` build is now the regression guard for any *future*
  `TypeKind`-exhaustive switch that forgets rows.
- Unit target `tur_typerow_unit` (`tests/compiler/test_typerow.c`, registered
  in both CMake files): 25 assertions covering construction, order-sensitive
  vs permutation equality, duplicate/nesting semantics, the empty row, and the
  printed `#row{...}` form. LSan-clean (uses `type_print` into an owned `Buf`,
  not the leaky composite-name path of `type_name`).

Purely additive: full fixture suite still green (no fixture constructs a row
yet -- surface syntax is Layer 3).

### Remaining layers (revised order)

3. **Surface syntax** per the PRE-1 decision (`#row{...}` reader form), with
   `tests/fixtures/hkt-row-*`
   fixtures. Must not perturb the existing tuple-sugar / binding-vector paths.
4. **Row-kinded type parameter.** Let a constructor parameter carry kind `[*]`
   so `Query` is functorial over a row. NOTE a second syntax sub-decision: the
   HKT param markers are `^f` -> `KIND_ARROW` and `^^f` -> `KIND_ARROW2`
   (`elab_types.c:1740-1772`; the fn-param path caps kind-vars at 8 and keys
   off a lowercase initial in `elab_fns.c:973-987`). A row-kinded parameter
   needs its own marker (e.g. `^[f]`), wired through both sites.
5. **Row operations** -- membership (`Pos in row`), union/`++` (query joins),
   propagation through partial application (`kind_of_type_app` already rejects
   applying a row; the *ops* are separate type-level functions).
6. **Codegen erasure + ECS fixture.** Rows are compile-time only and should be
   eliminated by `PASS_KIND_CHECK` (runs immediately after `PASS_ELABORATE`,
   before any codegen pass; see `src/runtime/pass.h`). Decide the runtime
   representation (erased to the element tuple / iterator), add the explicit
   `type_c_name` erase case, and add an ECS query fixture plus the `hkt-row-*`
   fixtures named in the validation section.

**Revised critical path:** PRE-3 + Layer 2 are the high-value, low-risk next
step (pure type-layer C work, unit-testable in isolation, no syntax decision
needed). PRE-1 only gates Layer 3 onward, so the syntax decision can be made in
parallel without blocking Layer 2.

When all layers land, `ecs-spice-plan.md` E1 (the variadic `query` form) is
unblocked and the `queryN` macro family becomes a deprecation alias per that
plan's D1.

## Related

- `docs/upcoming/v1/ecs-spice-plan.md` D1
- `docs/guides/hkt-guide.md`
- `docs/archive/` HKT plan (associated types deferred)
