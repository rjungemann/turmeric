# GADTs & Intersection/Union Types -- Follow-up Tasks

Remaining open items from [gadts-plan.md](gadts-plan.md) and
[intersection-union-types-plan.md](intersection-union-types-plan.md) as of
2026-05-16.

---

## Blocked on HKT

These items cannot proceed until the HKT kind system (`* -> *` kind inference
and kind-checking) is available.

---

### `equal-cong` (`stdlib/equal.tur`, G3/G4)

Implement:
```clojure
(defn equal-cong [^Functor f, eq : (Equal a b)] : (Equal (f a) (f b)) ...)
```
This requires the elaborator to understand a kind-`* -> *` type variable (`f`)
in a `defn` signature and to build `TY_APP` return types from it.

#### Step 1 — Register kind variables in `elab_defn` (`src/elab.c` ~line 6313)

- [x] Collect kind var names instead of silently skipping
- [x] Create inner scope before return-type parsing and push TY_TYVAR/KIND_ARROW bindings

Currently, when `elab_defn` encounters a `^f` annotation where the bare name
starts with a lowercase letter, it silently skips it:
```c
/* Kind variable — silently skip; no runtime parameter is created. */
continue;
```
Instead, intern the bare name (`f`) as a `Symbol` and push a local type
binding into the scope with `TY_TYVAR` / `hkt_kind = KIND_ARROW`.  This makes
`f` visible to `type_expr_from_form` when it later parses the return-type
annotation `(Equal (f a) (f b))`.  The binding carries no C-level parameter
(it is erased at runtime) — only the scope entry is needed.

#### Step 2 — Parse `(f a)` as `TY_APP` in `type_expr_from_form` (`src/elab.c` ~line 12640)

- [x] Verify F_LIST → TY_APP path handles TY_TYVAR/KIND_ARROW heads (already works via scope lookup + existing TY_APP construction)

`type_expr_from_form` handles `F_LIST` forms but currently does not produce
`TY_APP` when the head of the list resolves to a `TY_TYVAR` with
`hkt_kind = KIND_ARROW`.  Add a branch in the `F_LIST` path (after the union
and intersection checks, before the generic "type application" fall-through):

1. Look up the head symbol in the local scope.
2. If it resolves to a `TY_TYVAR` with `hkt_kind == KIND_ARROW` and the list
   has exactly two elements (head + one argument), call `type_make_app` (or
   construct the `TY_APP` node inline) to produce `(f a)` as a type.
3. Propagate the span from the form for diagnostics.

This is the same path used by `elab_definstance` when it encounters
`(result int)` — the difference is that here `f` is a local `TY_TYVAR` rather
than a globally-defined type constructor.

#### Step 3 — Skolem substitution through `TY_APP` nodes (`src/elab.c` / `src/types.c`)

- [x] Extend `gadt_resolve_type_from_form` F_LIST branch to return TY_TYVAR for kind-variable heads

In a `Refl` arm the `SkolemEnv` contains `a ~ b`.  The return type of
`equal-cong` is `(Equal (f a) (f b))`.  The type checker needs to verify that
`Refl` (which has type `(Equal a a)` after skolem substitution) satisfies
`(Equal (f a) (f b))`.

Verify that `type_subst_skolem` (or whichever function normalises types
against the active `SkolemEnv`) recurses into both `TY_APP.fn` and
`TY_APP.arg`.  If it currently only substitutes at the top level, extend it to
walk `TY_APP` nodes so that `(f a)` with skolem `a ~ b` becomes `(f b)` (or
the unification succeeds by substituting `a` → `b` inside the argument
position).  Search for usages of `TY_APP` in `src/types.c` and
`src/elab.c`'s `type_equiv` / `type_unify` to audit coverage.

#### Step 4 — Implement `equal-cong` in `stdlib/equal.tur`

- [x] Add `equal-cong` with full docstring

Once Steps 1–3 pass, add to `stdlib/equal.tur`:

```clojure
;;; equal-cong -- congruence: if a = b then (f a) = (f b) for any f : * -> *.
;;;
;;; Parameters:
;;;   eq -- a proof that a = b
;;;
;;; Returns:
;;;   A proof that (f a) = (f b).
;;;
;;; Example:
;;;   (equal-cong Refl)  ; => Refl
;;;
;;; Since: Phase G4 (requires HKT)
(defn equal-cong [^f, eq : (Equal a b)] : (Equal (f a) (f b))
  (match eq
    (Refl) Refl))
```

The body is `Refl` because in the `Refl` arm, the skolem `a ~ b` is active,
so `(f a)` and `(f b)` unify and `Refl : (Equal (f a) (f a))` satisfies the
goal `(Equal (f a) (f b))`.

#### Step 5 — New fixture `tests/fixtures/gadt-equal-cong/`

- [x] `input.tur`: import `equal.tur`, call `equal-cong Refl`, print result
- [x] `expected.stdout`: expected output
- [x] `flags`: `-Xgadt`

---

### Kind-check GADT type arguments (`src/elab.c`, `src/kind_check.c`, G1)

Verify that type arguments in constructor return-type annotations have the
correct kind.  Currently `elab_defgadt` validates that arguments are known
types but never calls `kind_of_type_app`; the `kind_check_pass` has no
`EX_DEFGADT` visitor at all.

#### Step 1 — Belt-and-suspenders check in `elab_defgadt` (`src/elab.c` ~line 11000)

- [x] Add kind check after `is_prim`/`is_param` guard in the return-type arg loop

In the existing "Change 4" loop that validates each type argument in the
constructor's return-type form, add a kind check after the
`primitives[]`/`is_param`/`is_prim` guard:

- If the argument is an `F_LIST` (a nested type application such as
  `(Succ n)`), resolve the head's kind with `kind_env_lookup` or the binding's
  `hkt_kind`.  If the head has `hkt_kind == KIND_ARROW` but the GADT parameter
  slot expects `KIND_STAR`, emit `TUR_E0012_KIND_MISMATCH` with a message like:
  `"defgadt: type argument '%s' in constructor '%s' return type has kind '* -> *' but kind '*' is expected"`.
- If the argument is a bare symbol that resolves to a type constructor with
  `hkt_kind != KIND_STAR` and the GADT parameter declaration does not use `^`,
  emit the same error.

For Phase G1, all GADT type parameters are assumed kind `*`, so any non-`*`
argument in a return-type annotation position is an error.

#### Step 2 — Add `EX_DEFGADT` visitor to `kind_check_pass` (`src/kind_check.c`)

- [x] Add `EX_DEFGADT` case to `kind_check_expr` switch

`kind_check_pass` walks `EX_TYPECLASS_DEF` and `EX_INSTANCE_DEF` nodes but
ignores `EX_DEFGADT`.  Add a case for `EX_DEFGADT` in the top-level visitor
loop (the same loop in `kind_check_pass` that currently handles
`EX_TYPECLASS_DEF`):

1. Retrieve the `AdtDef *` from `e->as.defgadt_.def`.
2. For each `CtorDef *ctor` in `def->constructors`:
   a. If `ctor->result_type_form == NULL`, skip (plain `defdata` constructor).
   b. Walk the type-argument sub-forms of `result_type_form` (indices 1..len-1
      of the `F_LIST`).
   c. For each sub-form that is itself an `F_LIST` (i.e. a type application),
      look up the head type's kind via `kind_env_lookup` and call
      `kind_of_type_app(fn_type, arg_type, span)`.  This validates that the
      type application is well-kinded.
   d. Emit `TUR_E0012_KIND_MISMATCH` on any kind violation (same error as
      `elab_definstance` kind checks).
3. This pass runs *after* elaboration, so kind information is already populated
   on type bindings — no forward-reference issue.

#### Step 3 — New fixture `tests/fixtures/errors/gadt-kind-error/`

- [x] `input.tur`: `(defgadt Bad [] (MkBad : (Bad Pair)))` -- kind-`* -> *` in kind-`*` slot
- [x] `expected.diag`: `TUR_E0012_KIND_MISMATCH` error (substring match)
- [x] `flags`: `-Xgadt`

---

## Type System / Elaborator

- [x] **Redundant arm warnings** (`src/elab.c`, G0): emit a warning when a `match`
  arm can never be reached (covered by an earlier arm). Currently only missing
  arms produce an error. Also added arm body type consistency check (all arms
  must return the same type) and fixed `src/emit.c` to skip duplicate case
  labels for redundant constructor arms. Fixtures: `match-redundant-arm`
  (happy-path, verifies the program still runs) and
  `errors/match-arm-type-mismatch` (negative fixture for the type check).

- [x] **`rank()` helper** (`src/types.h`, G1): expose a helper that reports
  `TY_ADT` with the `is_gadt` flag as requiring refinement -- supports future
  tooling and the HRT rank-checking path. Implemented as
  `type_requires_refinement(Type t)` inline in `src/types.h`.

- [ ] **`TY_SKOLEM_EQ` TypeKind** (`src/types.h`, G2): the current implementation
  uses named `TY_TYVAR` + `SkolemEnv` as a stand-in. A first-class
  `TY_SKOLEM_EQ` kind would make skolem equalities explicit in the type
  representation, simplifying `type_equiv`/`type_unify` and improving error
  messages.

- [ ] **Implicit union widening** (`src/elab.c`, IT1): a value of type `A` should
  be implicitly widened to `(A | B)` at call sites and return positions without
  requiring an explicit coercion. The elaborator must insert coercion nodes;
  codegen must construct the tagged-union wrapper.

---

## Error Messages

- [x] **GADT type mismatch shows active skolems** (`src/elab.c`/`src/diag.c`, G2):
  when a type mismatch occurs inside a GADT match arm, the error should display
  the active skolem equalities in scope (e.g. `note: in this arm a ~ int`).
  Implemented: when arm body type is inconsistent with the established result
  type in a GADT arm that has active skolem bindings, a `DIAG_NOTE` is emitted
  first naming the constructor and listing `a ~ T` refinements. Also emitted
  as a note when body elaboration fails (`body == NULL`) inside a GADT arm with
  non-empty skolem env. Fixture: `errors/gadt-arm-skolem-context`.

- [x] **GADT constructor context in type mismatch** (`src/diag.c`, G1): type
  mismatch errors occurring inside a GADT elaboration should name which
  constructor's return-type annotation caused the refinement. Implemented via
  `g2_current_ctor` field on the `Elab` struct (set/restored per GADT arm);
  the active constructor name appears in the skolem note emitted alongside
  type mismatch errors.

- [x] **`tur explain` GADT codes** (G4): `TUR_E0010`--`TUR_E0019` were originally
  reserved for GADT diagnostics but were repurposed for thread-safety,
  kind-mismatch, typeclass, and continuation errors -- all of which already
  have `tur explain` entries in `src/diag.c`. No GADT-specific codes remain
  unregistered in this range.

---

## Standard Library

- [x] **`stdlib/gadt-vec.tur`** (G4): created `stdlib/gadt-vec.tur` with a
  GADT-based singly-linked int list using a phantom type parameter (`Vec n`).
  Includes `vec-nil`, `vec-cons`, `vec-len`, `vec-sum`, `vec-head-or`,
  `vec-tail`, `vmap`, and `vzip-with`. All functions have full `;;;` docstrings.
  Fixture: `tests/fixtures/gadt-stdlib-vec-stdlib/` (passes with `-Xgadt`).
  Note: the original `gadt-stdlib-vec` fixture exercises the same functions
  inline; this new stdlib file extracts them for reuse. The planned
  `VNil`/`VCons` API from the spec uses `int` elements (HKT polymorphism over
  element type requires `* -> *` kind variables, deferred to HKT phase).

- [x] **`stdlib/result.tur` GADT edition** (G4): decided to keep `Result e a`
  as a plain `defdata`. `Result` has two constructors (`Ok`/`Err`) with no
  type-refinement behavior -- a `defgadt` would add annotation overhead with no
  benefit. The existing `stdlib/result.tur` (C-struct approach, production
  quality) is retained unchanged.

---

## Intersection/Union Type Codegen (IT4)

- [x] **Tagged union codegen** (`src/emit.c`, IT4): `tur_tagged_t` struct
  (`{int64_t tag; int64_t val;}`) defined in the C preamble. `type_c_name(TY_UNION)`
  now returns `"tur_tagged_t"`. `EX_UNION_INJECT` AST node wraps member values
  with `TUR_TAG(member_idx, val)` at call sites. `EX_MATCH` on union-typed
  scrutinees emits an `if/else if` chain checking `TUR_GETTAG()` against the
  member index stored in `MatchPattern.union_member_idx`. All 365 `expected.c`
  snapshots regenerated. New fixtures: `union-types-match` (2-way dispatch),
  `union-types-threeway` (3-way dispatch).

- [x] **Boxing codegen for `any`** (`src/emit.c`, IT4): `type_c_name(TY_ANY)` now
  returns `"tur_tagged_t"`. When a value of type `T` flows into `any` at a call
  site, `EX_UNION_INJECT` wraps it with `TUR_TAG(TypeKind_of_T, val)`, where
  `TypeKind_of_T` is the numeric `TypeKind` enum value. New fixture:
  `union-types-any`. Unboxing via `(cast)` and `(type-of)` are deferred (see
  below).

- [ ] **Gradual typing stdlib** (`src/elab.c`/`stdlib/`, IT4):
  - `(cast x : T)` -- runtime downcast from `any`; check `TUR_GETTAG(x) == TypeKind_T`
    and return `(option T)` (some with unboxed value, or none on tag mismatch)
  - `(type-of x)` -- returns a runtime type tag as a cstr (maps `TUR_GETTAG(x)` to
    a string via a lookup table)
  Infrastructure (`tur_tagged_t`, boxing injection) is now in place; requires
  new builtin elaboration + new stdlib functions.

- [ ] **Typeclass instance intersection on unions** (`src/elab.c`, IT4): when
  `x : (A | B)`, allow calling typeclass methods that are available on *all*
  union members directly, without requiring a `match`. The elaborator computes
  the instance intersection at the union type site and generates a tag-dispatched
  call. Requires `tur_tagged_t` codegen (now done) plus elaborator changes to
  compute method-intersection sets for union types.

---

## ADT-as-Union Sugar (stretch, requires both `-Xgadt` and `-Xunion-types`)

These items are a stretch goal gated on both flags being active simultaneously.
See gadts-plan.md G4 and intersection-union-types-plan.md IT4 for background.

- [ ] Desugar `(defdata Option [a] (None) (Some a))` to a union type
  `(None | (Some a))` internally.
- [ ] Desugar `(defdata Result [e a] (Ok a) (Err e))` to `(Ok a | Err e)`.
- [ ] Plain `defdata` without `-Xunion-types` continues to emit the existing
  tagged-union C struct; exhaustiveness and pattern matching are unchanged.

---

## Documentation

- [ ] **Cross-plan references** (G4): update `docs/higher-ranked-types-plan.md`
  §Non-Goals and `docs/higher-kinded-types-plan.md` §Non-Goals to reference
  gadts-plan.md.

- [ ] **Language reference** (G4): add a GADT section to the language reference
  manual covering `defgadt`, `match` refinement, `Equal`, `coerce`, and `(~)`
  constraint notation.

- [ ] **Cookbook entries** (G4): write cookbook-style examples for: typed AST
  interpreter, length-indexed vector, type-safe printf format strings, equality
  witnesses.

---

## Testing & Performance

- [ ] **Property tests** (G4): `eval-expr` produces correct types and values for
  randomly generated well-typed expression trees.

- [ ] **Fuzz tests** (G4): fuzz the type checker with randomly generated `defgadt`
  definitions and `match` expressions; verify no crashes or unsound results.

- [ ] **Performance benchmarks** (G4/IT4):
  - GADT `match` dispatch vs. plain ADT `switch`
  - Compile-time overhead of skolem environment management
  - Verify `coerce` emits zero instructions under `-O2`
  - Tagged union emission overhead vs. existing ADT codegen (`any`/IT4)
  - Document findings in `docs/gadts-guide.md`
