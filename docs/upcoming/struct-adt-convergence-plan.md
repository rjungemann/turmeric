---
title: Struct/ADT Convergence Plan
category: Planning
description: Unify structs and ADTs by treating `defstruct S [...]` as sugar for a single-variant ADT `defadt S (S [...])`, with shared codegen (flat layout for single-variant, no tag word), record-style ADT variants, `match` accepting struct values, and one named-field-constructor surface across both.
---

# Struct / ADT Convergence -- Plan

## Implementation status (2026-06-25)

**CONV-S1 by-value merge: bridging slices B1-B3 landed; B4 (recursive HKT
fat-closure ABI) shipped and graduated 2026-06-25 (see archived
`b4-fat-closure-byvalue-adt-abi-plan.md`). The s1-bridging findings doc has
been archived. CONV-S1 proper (`defstruct` lowering to `defadt`) is landing
incrementally in [`defstruct-as-defadt-plan.md`](defstruct-as-defadt-plan.md);
the parametric `:heap` ABI tail is tracked in
[`parametric-adt-byvalue-plan.md`](parametric-adt-byvalue-plan.md) step 5.**


The **surface-level** convergence has landed (each step tested, full suite
green):

- **CONV-S0 -- record-style `defdata` variants.** `(Circle [radius : float])`
  parses, constructs positionally, and `match`-binds by position or by name.
- **CONV-S4 (keyword half) -- keyword construction.** `(Circle :radius 2.0)`,
  reordering free, mirrors `make-struct` KW-V0.
- **CONV-S3 -- `match` on struct values.** Desugars a struct scrutinee to a
  `let` over the existing field accessors (no match-emit change).
- **CONV-S5 -- `:copy` traverses ADT variants.** A `:copy` sum with a
  non-copy field is rejected, pinpointing the field and variant.
- **CONV-S7 (partial) -- docs.** Record variants and match-on-struct are
  documented in the sum-types and structs guides.

**CONV-S2 (contained) -- landed.** Single-variant, non-GADT ADTs now codegen
to a *flat product* layout: the `tur_adt_<Name>` typedef carries no `int tag`
word, the per-constructor allocator stores no tag, and `match` enters the sole
arm unconditionally (no `switch (__scrut->tag)`). Multi-variant ADTs and GADTs
(whose tag may drive return-type refinement) keep the tagged-union layout
unchanged. The gate is a single predicate, `adt_is_flat_product()`
([`types.h`](../../src/compiler/types.h)), shared by every tag-emitting and
tag-reading codegen site so the typedef, constructors, and match stay in
lockstep:

- typedef + ctor emission -- `emit_adt_typedef_and_ctors`
  ([`emit_module.c`](../../src/compiler/emit_module.c)), the early-file path in
  the same file, and the monomorphized ADT-app emitter
  ([`types.c`](../../src/compiler/types.c) `emit_registered_adt_app_rec`).
- match emission -- the if-chain path in
  ([`emit_expr.c`](../../src/compiler/emit_expr.c)) is reused for flat ADTs
  with the tag test dropped.

This is the *contained* reading of CONV-S2: the single-variant ADT is flat
(no tag word, no discriminant load) but still flows through the heap-pointer
int64 carrier ABI -- it is **not** yet byte-identical to a by-value
`defstruct` (that requires CONV-S1's representation merge, below). Fixtures:
`conv-single-variant-flat` (snapshot: tagless layout), `conv-multi-variant-tagged`
(snapshot: tag preserved), `errors/conv-copy-mixed-variant-rejects` (CONV-S5).

**Field access + CONV-S4(`with`) on single-variant record ADTs -- landed.**
A single-variant, non-GADT record ADT now exposes the full struct surface:

- **Field access** -- `(.field v)` and `(. v field)` resolve against the sole
  variant's named fields. The dot-accessor handler in
  ([`elab_typeclasses.c`](../../src/compiler/elab_typeclasses.c)) gained an ADT
  branch alongside the `TY_STRUCT` one; it builds an `EX_GET_FIELD` whose `def`
  is NULL and whose `adt_def`/`adt_ctor` carry the variant, and emit
  ([`emit_expr.c`](../../src/compiler/emit_expr.c)) reads
  `((tur_adt_X *)v)->as.Ctor._<idx>`. (A typed receiver is required, exactly as
  for structs.)
- **`with`** -- `elab_with_record_adt`
  ([`elab_structs.c`](../../src/compiler/elab_structs.c)) lowers
  `(with v [f val ...])` to `(let [G v] (Ctor <f0> ...))`, filling unchanged
  fields with `(.field G)`; it requires a `:copy` type, mirroring the struct
  path. Fixture: `conv-with-record-variant`.

**Deferred -- the full representation merge** (a dedicated effort; structs are
by-value flat C structs while ADTs are heap-pointer carriers, and
ctor/ABI codegen all branch on that):

- **CONV-S1 -- `defstruct` lowers to a single-variant `defdata`.** With CONV-S2
  the tag-word cost is gone, but a true merge still has to make single-variant
  ADTs flow *by value* (today they are heap-pointer int64 carriers) so the
  lowered struct is byte-identical -- touching the ABI, monomorphization,
  drop-glue, and ~57 `defdata` fixtures. A spike of the by-value core (which
  works for typed receivers) found that suite-green requires a full
  byval<->carrier bridging merge at every crossing (int-typed params, ADT/GADT
  fields, closure/HKT args) plus untyped-param inference; the exact sites and a
  suite-green B1-B4 decomposition are catalogued in
  [`struct-adt-convergence-s1-bridging-findings.md`](struct-adt-convergence-s1-bridging-findings.md).
- **`with` on a *narrowed* multi-variant ADT.** The single-variant case landed
  above; `(with v [...])` inside a `match` arm that narrowed a multi-variant
  ADT to one record variant needs variant narrowing in the type system (the
  arm scrutinee still has the full ADT type today) and stays out of scope, as
  the plan's "Open questions" note.
- **CONV-S6 -- diagnostic wording pass** -- best done once the merge settles.

## Context

Today Turmeric has two parallel notions of product-shaped data:

- **`defstruct S [...]`** -- a named-field product type with positional
  construction (`make-struct S ...`), field accessors (`(. s field)`
  and `(.field s)`), `:copy` ownership attributes, and (after the
  struct-ergonomics plan) auto-bound constructors, keyword
  construction, and `with` functional update.
- **`defadt T (Variant1 [a b]) (Variant2 [c]) ...`** -- a sum of
  positional-field variants, with constructors auto-bound per variant,
  pattern-matching via `match`, and no notion of "named fields on a
  variant payload."

These are two surfaces for one underlying idea: **a tagged disjoint
union of named-field products.** A struct is the degenerate case
(one variant, named fields). A positional ADT variant is the
degenerate case (one of many variants, anonymous fields). The
intersection -- a sum of named-field products, like Rust's
`enum Foo { Bar { x: int, y: int }, Baz { name: cstr } }` -- has no
representation in Turmeric today.

The companion plan
[`docs/upcoming/struct-ergonomics-plan.md`](../struct-ergonomics-plan.md)
specified CTOR/KW/WITH in terms of "named-field constructors" and
"single-variant-known contexts" precisely so this convergence does
not require revisiting them. The work in this plan is the *codegen,
type-system, and surface-syntax* unification underneath.

## Goals

1. **CONV-S0 -- record-style ADT variants.** `defadt` accepts a
   variant whose payload is a named-field set:

   ```turmeric
   (defadt Shape
     (Circle [radius : float])
     (Rect   [w : float h : float])
     (Named  [name : cstr inner : Shape]))
   ```

   Field access on a narrowed variant value works via `(. v field)`
   and `(.field v)`. Pattern-matching binds by name or by position
   (your choice, both accepted).
2. **CONV-S1 -- `defstruct` is sugar for a single-variant ADT.**
   `(defstruct S [...])` lowers to `(defadt S (S [...]))` at the
   elaboration boundary. The type name `S` and the sole-variant
   constructor `S` share an identifier, exactly as Haskell's
   `data Person = Person { ... }`. Existing struct callsites
   continue to work; the difference is internal.
3. **CONV-S2 -- single-variant flat-layout specialization.** Codegen
   detects single-variant ADTs and emits the same flat C struct
   layout `defstruct` produces today: no tag word, no discriminant
   load on field access. Multi-variant ADTs keep their tagged
   representation. This is a codegen optimization on the unified
   IR, not a semantic compromise.
4. **CONV-S3 -- `match` accepts struct values.** `(match s (S name age) ...)`
   works on a struct because a struct *is* a single-variant ADT.
   The struct-narrowed accessor path and the match-narrowed accessor
   path become the same path.
5. **CONV-S4 -- KW-V0 / WITH-V0 lift to record variants.** The
   keyword construction and `with` macro from the struct-ergonomics
   plan apply to record-style ADT variants without source changes,
   because they were specified over named-field constructors and
   single-variant-known contexts.
6. **CONV-S5 -- `:copy` lifts to ADTs.** `defadt` accepts `:copy`
   (or per-variant `:copy`) declaring that values of the type may
   be implicitly copied. The struct ownership rules generalize
   variant-by-variant.
7. **CONV-S6 -- one diagnostic surface.** Construction, field
   access, pattern matching, and update errors reference "variant"
   uniformly; struct-specific diagnostics either get reworded or
   become wrappers around the variant-level ones.
8. **CONV-S7 -- documentation alignment.** The struct guide and the
   ADT guide are reconciled into one "product and sum types" guide,
   with cross-references; the two-keywords story (`defstruct` /
   `defadt`) is documented as syntactic surface, not as two
   different type-system features.

Non-goals:

- **Removing `defstruct`.** It stays as the recommended surface for
  single-variant named-field products. Users should not have to spell
  out `(defadt Person (Person [...]))` for the common case.
- **Removing positional-field variants.** Tuple-style variants stay
  as a first-class form for ADTs whose fields don't benefit from
  names (`Cons`, `Pair`).
- **Anonymous structs / row polymorphism.** A separate direction;
  this plan is about unifying *named* product/sum surfaces, not
  introducing structural typing.
- **GADT-specific syntax changes.** GADT refinement machinery is
  orthogonal; this plan does not touch return-type indexing.

## Design

### CONV-S0 -- record-style ADT variants

Extend `defadt`'s variant parser to accept both forms:

```turmeric
(defadt Shape
  (Circle [radius : float])                ; record-style (named slots)
  (Rect   w : float h : float)             ; positional-style (anonymous slots)
  (Square side : float))                   ; positional shorthand
```

Decision rules:

- A `[ ... ]` block introduces record-style fields. Each field has
  a name and a type, with the same `name : type` shape `defstruct`
  uses.
- Bare `name : type ...` after the variant name introduces
  positional fields. The slot names are still recorded (for
  accessor generation and pattern binding by name as an
  ergonomic option) but the surface treats them as positional.
- A variant must commit to one style; mixing positional and
  record-style in one variant is an error.

Both styles produce auto-bound constructors. A record-style variant
also accepts keyword construction per KW-V0 from the struct-ergonomics
plan.

### CONV-S1 -- `defstruct` as sugar

At the elaboration boundary:

```turmeric
(defstruct Person :copy [name : cstr age : int])
;; lowers to
(defadt Person :copy
  (Person [name : cstr age : int]))
```

Constraints:

- The type name and the sole-variant name are identical. This is
  the Haskell convention; it makes the auto-bound constructor and
  the type ascription share an identifier without ambiguity
  (positional disambiguation in type vs. value contexts is already
  how the elaborator distinguishes them).
- `:copy` on the `defstruct` becomes `:copy` on the `defadt` (and,
  equivalently, `:copy` on the sole variant). Other struct
  attributes (`:no-auto-ctor` etc.) lower similarly.
- The lowering happens at a defined elaboration phase. All
  downstream phases (type checking, codegen, docs) see the
  ADT representation; struct-specific code paths are deleted, not
  preserved as a parallel branch.

### CONV-S2 -- flat-layout specialization

Codegen detects "single-variant ADT" at the type level and emits the
same C struct layout `defstruct` produces today:

- No tag word.
- Direct field access via the C struct member, not via a
  `discriminant + payload` indirection.
- `:copy` semantics unchanged: trivially-copyable C struct.

Multi-variant ADTs keep their existing representation (tag + union
of payloads, or whichever representation `defadt` codegens today).
The specialization is a pure optimization: it does not change
observable semantics, only generated C.

Verify with a codegen-snapshot fixture per case:

- `tests/fixtures/conv-single-variant-flat/` -- generated C for a
  `(defadt S (S [a : int b : cstr]))` is byte-identical to the
  generated C for `(defstruct S [a : int b : cstr])`.
- `tests/fixtures/conv-multi-variant-tagged/` -- two-or-more-variant
  ADTs keep their tagged representation; tag layout unchanged.

### CONV-S3 -- `match` on structs

Because a struct lowers to a single-variant ADT, `match` accepts
struct values with no new machinery:

```turmeric
(defstruct Person :copy [name : cstr age : int])

(let [p (Person "Bob" 40)]
  (match p
    (Person name age)
      (println name)))
```

The exhaustiveness checker treats the single-variant case as
trivially exhaustive. Match expressions on structs that bind by name
(once record-style variants are supported) reuse the same binding
machinery as record-variant matches.

### CONV-S4 -- KW-V0 / WITH-V0 lift to record variants

No additional design required; the struct-ergonomics plan already
specified these over named-field constructors and
single-variant-known contexts. Add fixtures to confirm:

- `tests/fixtures/conv-kw-record-variant/` -- keyword construction
  for a record-style variant works identically to keyword
  construction for a single-variant ADT (i.e. a struct).
- `tests/fixtures/conv-with-narrowed-variant/` -- `(with v [f val])`
  inside a `match` arm that narrowed to a record variant produces a
  new variant value with the override applied; rejected outside a
  narrowing context with a clear diagnostic.

### CONV-S5 -- `:copy` on ADTs

`defadt` accepts `:copy` at the ADT level (applies to all variants) and
optionally per-variant. The trivially-copyable check on a `:copy`
ADT is "every variant's payload is `:copy`-compatible"; a mixed ADT
where one variant has a non-`:copy` field cannot itself be `:copy`,
and elaboration rejects this with a diagnostic pointing at the
offending field.

`:copy` on the lowered-from-struct case is a no-op transformation
(the struct was already `:copy`; the lowered single-variant ADT is
too).

### CONV-S6 -- diagnostic surface

Audit the existing struct diagnostics:

- "field not found on struct S" -> "field not found on variant S
  of type T" (with T = S for the single-variant case, so the
  message stays readable).
- "missing field in struct construction" -> "missing field in
  variant construction" (similar).
- The struct-ergonomics plan's four KW-V0 diagnostics get
  re-described in terms of "named-field variant" but their codes
  do not change.

Where a struct-specific diagnostic is more readable for the
single-variant case (because mentioning "variant" adds noise),
keep the struct-flavored wording and produce it from the
variant-level path when the type has a single variant. Wording
choices, not new infrastructure.

### CONV-S7 -- documentation alignment

- Merge the struct guide and the ADT guide into a single "Product
  and Sum Types" guide under `docs/guides/`.
- Document `defstruct` as sugar for the single-variant ADT case.
- Document record-style variants alongside positional variants.
- Cross-reference KW-V0, WITH-V0, and the auto-bound constructor
  surface from one place.

## Migration and compatibility

The convergence is internal to elaboration and codegen. From the
surface, every existing program compiles unchanged:

- Existing `defstruct` declarations work.
- Existing `make-struct` callsites work.
- Existing field accessors (`(. p name)`, `(.name p)`) work.
- Existing `:copy` semantics work.

New surface added:

- Record-style ADT variants.
- `match` on struct values.
- KW-V0 / WITH-V0 on record-style variants (free with the
  struct-ergonomics plan's machinery).

No surface removed. The convergence is additive on the surface and
consolidating in the internals.

## Codegen risks

- **Single-variant detection must be reliable.** Any failure to
  detect "this ADT has one variant -> use flat layout" silently
  imposes a tag-word cost on every struct. Add a fixture that
  asserts the flat layout is reached by `defstruct`-originated
  types *and* hand-written single-variant `defadt` types.
- **Tagged-multi-variant codegen unchanged.** No existing fixture
  for multi-variant ADTs may shift its snapshot under this work.
  Regenerate snapshots only for fixtures whose codegen genuinely
  moves (i.e. struct-originated, where the post-lowering ADT path
  produces byte-identical C to the pre-lowering struct path -- so
  in practice no snapshots should move at all).
- **`:copy` analysis must traverse variants.** A multi-variant ADT
  marked `:copy` requires every variant to be `:copy`-compatible;
  a regression here would silently accept a non-`:copy` field.
- **Pattern-match codegen on single-variant.** `match` on a
  single-variant value should compile to direct field reads, not
  to a tag-test-then-payload-read. Verify with a snapshot.

## Type-system risks

- **Naming collision between type and constructor.** The
  Haskell-style "type and sole variant share a name" works only if
  the elaborator already distinguishes type and value contexts by
  position. Verify by writing a fixture that exercises both
  contexts in close proximity; if the disambiguation is fragile,
  fix it before lowering `defstruct` to `defadt`.
- **Inference for record-style variants.** Constructing
  `(Circle :radius 1.0)` needs the same parametric inference path
  `make-struct` uses today. No new machinery if KW-V0 is built on
  the same lookup, but verify with a parametric ADT fixture.
- **Exhaustiveness checking.** Single-variant `match` is trivially
  exhaustive; the checker must recognize this and not require a
  catch-all. Verify with a fixture.

## Fixtures and tests

- `conv-single-variant-flat/` -- single-variant ADT produces
  flat-layout C, byte-identical to the struct case.
- `conv-multi-variant-tagged/` -- multi-variant ADT keeps tagged
  layout; snapshot unchanged from before.
- `conv-record-variant-construct/` -- record-style variant accepts
  positional and keyword construction.
- `conv-record-variant-match/` -- pattern match binds record-variant
  fields by name and by position.
- `conv-struct-as-adt-match/` -- `match` on a struct value works
  with the sole-variant name.
- `conv-kw-record-variant/` -- KW-V0 lifts unchanged.
- `conv-with-narrowed-variant/` -- WITH-V0 inside a `match` arm.
- `conv-copy-mixed-variant-rejects/` -- a `:copy` ADT with a
  non-`:copy` field is rejected with the variant pinpointed.
- `conv-defstruct-lowering/` -- semantic equivalence: every
  observable behavior of `(defstruct S [...])` matches
  `(defadt S (S [...]))` for the same fields.

## Order of work

The dependencies form a clear graph; suggested landing order:

1. **CONV-S0** -- record-style ADT variants. Independent of
   everything else; ships the new variant surface. Fixtures:
   `conv-record-variant-construct`, `conv-record-variant-match`.
2. **CONV-S5** -- `:copy` on ADTs. Lift the existing struct
   `:copy` analysis to traverse variants. Fixture:
   `conv-copy-mixed-variant-rejects`.
3. **CONV-S2** -- flat-layout codegen for single-variant ADTs.
   Independent of CONV-S1 (a hand-written
   `(defadt S (S [...]))` is the test vehicle). Fixtures:
   `conv-single-variant-flat`, `conv-multi-variant-tagged`,
   pattern-match-on-single-variant snapshot.
4. **CONV-S1** -- `defstruct` lowers to single-variant `defadt`.
   Builds on S2 (otherwise lowering would impose a tag cost on
   every struct). Fixture: `conv-defstruct-lowering`. Verify the
   whole existing fixture suite is unchanged.
5. **CONV-S3** -- `match` on struct values. Builds on S1 (the
   struct *is* an ADT at this point, so `match` already works;
   this step is mostly fixture coverage plus exhaustiveness check
   adjustment). Fixture: `conv-struct-as-adt-match`.
6. **CONV-S4** -- KW-V0 / WITH-V0 on record-style variants.
   Builds on S0 + the struct-ergonomics plan. Fixtures:
   `conv-kw-record-variant`, `conv-with-narrowed-variant`.
7. **CONV-S6** -- diagnostic wording pass. Final, after the
   internal unification has settled. No fixtures (covered by
   error-message snapshots if those exist; otherwise spot-check
   with a focused fixture per re-worded diagnostic).
8. **CONV-S7** -- guide merge and documentation. After all of
   the above land.

Each step ships with its own fixtures; the test suite
(`bash tests/run.sh`, timeout 600000) gates each landing. Snapshot
regenerations happen *in the same commit* as the codegen change that
caused them.

## Open questions

- **Should `defstruct` ultimately be removed in favor of a record
  variant of `defadt`?** This plan says no -- `defstruct` is the
  ergonomic surface for the single-variant case and should stay. But
  the question may resurface once both surfaces have been live for a
  while; revisit at v2 boundary, not v1.
- **Anonymous structs / row polymorphism.** Out of scope here, but
  whoever picks this up later should make sure the named-field
  variant infrastructure does not foreclose adding structural typing
  on top.
- **`with` on multi-variant ADTs.** Strictly out of scope: WITH-V0
  requires a single-variant-known context. A future "tagged
  functional update" feature (`with`-with-fallback-arms) is a
  separate plan; this convergence neither blocks nor enables it.
