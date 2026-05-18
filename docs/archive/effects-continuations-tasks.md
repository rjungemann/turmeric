# Effects and Continuations -- Consolidated Task List

> Drawn from: `effect-rows-plan.md` (ER0--ER6), `effect-types-row-polymorphism-plan.md` (ET0--ET4),
> `linear-continuations-plan.md` (LC0--LC3), `multishot-continuations-plan.md` (MS0--MS4).
>
> Completed phases (ER0--ER5) are omitted. Tasks are ordered by dependency.
>
> **Last updated:** 2026-05-17

---

## Legend

- **ER6** -- Effect row polish / ergonomics (near-term, no prerequisites)
- **ET0--ET1** -- Effect rows in type positions + expression-level checking (can run parallel to HRT)
- **GATE** -- Gating prerequisite before ET2 can start
- **ET2** -- Effect polymorphism (`forall [e]`) -- requires HRT Phase HRT1
- **ET3** -- Handler typing and first-class handlers
- **ET4** -- Stdlib, effect hierarchy, tooling, `-Xeffect-types` flag
- **LC0--LC3** -- Linear continuations -- requires LT0--LT4 + ET0--ET4
- **MS0--MS4** -- Multi-shot continuations -- requires LC0--LC3

---

## ER6 -- Integration, Polish, and Surface Ergonomics

### ER1 remaining -- `--strict-effects` and pure-function enforcement

- [x] Add `--strict-effects` flag; store in `CompilerOptions`
- [x] Under `--strict-effects`, unannotated functions emit `TUR-W0030` when inferred row is non-empty
- [x] Under `--strict-effects`, calling unannotated function from annotated function treats callee's inferred row as declared (no suppressed-warning gap)
- [x] Confirm `#{}` round-trip works for closures and inner `defn`s via `effect_row_is_subset`
- [x] Inner `(fn [...] ...)` literals inherit enclosing function's row when no annotation is given
- [x] `(fn [...] #{} ...)` closure annotation: emit `TUR-E0009` if closure body performs any effect
- [ ] Over-annotation warning: emit `TUR-W0031` when declared row is non-empty but inferred row is empty
- [x] `main` purity: under `--strict-effects`, treat unannotated `main` as having an implicit open row
- [x] `main [] #{}` convention: emit `TUR-E0009` if any effect escapes `main`
- [x] Fixture: `effect-pure-closure.tur` -- closure with `#{}` annotation rejected when body performs an effect
- [x] Fixture: `effect-strict-mode.tur` -- `--strict-effects` warns on unannotated effectful function
- [x] Fixture: `effect-main-pure.tur` -- `main [] #{}` accepted when all effects handled
- [x] Fixture (negative): `errors/effect-main-leak.tur` -- `main [] #{}` rejected when effect escapes
- [ ] Fixture (negative): `errors/effect-over-annotated.tur` -- `TUR-W0031` for declared but never-performed effect

### ER2 remaining -- Row-variable enforcement and higher-order propagation

- [x] Wire `effect_row_unify` into `collect_effects_in_expr` for `EX_CALL` nodes: bind row variable `e` to the inferred row of the actual argument at each call site
- [x] Per-call-site binding only (consistent with existing `EffectRowSubst` design; not global)
- [x] When function-typed argument is a closure literal, re-analyse its body in the current substitution scope
- [x] After unifying, apply substitution to callee's declared row and merge into caller's inferred row
- [x] When actual argument is a named function (not closure), look up its declared or inferred row and unify
- [x] A `defn` annotated `#{e}` that internally calls `(perform (Write ...))` adds `Write` to `e`'s binding
- [ ] Under `--strict-effects`, emit `TUR-W0032` when row variable `e` is always concrete (hint to use explicit row)
- [x] Fixture: `effect-row-ho.tur` -- `map`-style higher-order function with row variable; caller's inferred row includes mapped function's effects
- [x] Fixture: `effect-row-compose.tur` -- two functions with different row variables composed; union propagates correctly
- [x] Fixture: `effect-row-var-unused.tur` -- row variable never bound stays open
- [x] Fixture (negative): `errors/effect-row-var-mismatch.tur` -- row variable bound to `{Write}` but caller declares `#{}` yields `TUR-E0009`

### ER3 remaining -- Typeclass method effect rows

- [x] Verify `defclass` method `#{...}` annotations are stored in `MethodSig` in `src/typeclass.h`; add `effect_row` field if missing
- [x] In `effect_check_pass`, include `definstance` method bodies (`EX_DEFINSTANCE` nodes); declared row is the typeclass method's row
- [x] Emit `TUR-E0009` when instance method body's inferred row is not a subset of the declared method row
- [x] In `collect_effects_in_expr`, look up method's declared row from the typeclass when resolving a method call; merge into caller's row
- [x] Error: calling effectful method from `#{}` function yields `TUR-E0009`
- [ ] If `defclass` provides a default method body, check it against the method's declared effect row
- [x] Fixture: `typeclass-effect-row-enforced.tur` -- instance method that violates declared row produces `TUR-E0009`
- [x] Fixture: `typeclass-effect-row-caller.tur` -- calling an effectful method propagates the method's row to the caller
- [x] Fixture: `typeclass-effect-row-pure-ctx.tur` -- calling an effectful method from `#{}` function is rejected
- [ ] Fixture: `typeclass-effect-row-default.tur` -- default method body checked against declared row

### ER4 remaining -- Effect row subtyping in function types

- [x] In `type_is_subtype` (or equivalent), for `TY_FN` types: return row is covariant (`r1_ret ⊆ r2_ret`); argument rows are contravariant (`r2_param ⊆ r1_param`)
- [x] Assignments: `let f : (fn [] #{Write} :nil) = g` where `g : (fn [] #{} :nil)` is accepted
- [x] Capability fields: struct field typed `(fn [] #{Write} :nil)` can hold a function with row `#{}` (narrower satisfies wider)
- [x] Higher-order arguments: `(run-twice g)` accepts `g : (fn [] #{Write} :nil)` where `run-twice` expects `(fn [] #{Write Log} :nil)`
- [x] Two function types with different effect rows are not equal but may be subtypes; update elaborator's type unification to accept subtype assignments
- [x] Struct fields typed `(fn [] #{e} :T)`: bind row variable `e` at construction time and propagate at call sites
- [x] `(defstruct Runner [run : (fn [] #{e} :nil)])` is parametric in `e`; check caller's declared row when `.run` is called
- [x] Fixture: `effect-subtype-assign.tur` -- narrow-row function assigned to wide-row variable is accepted
- [x] Fixture: `effect-subtype-ho.tur` -- narrow-row function passed to wide-row higher-order parameter is accepted
- [x] Fixture: `effect-subtype-capability.tur` -- capability field accepts narrow-row function
- [x] Fixture (negative): `errors/effect-subtype-violation.tur` -- wide-row function assigned to narrow-row variable is rejected

### ER6 -- `try-with` sugar

- [x] Implement `(try-with body handler)` as macro in `stdlib/effects.tur` or special form in `src/reader.c`; desugar to `(reset (handle body handler))`
- [x] Fixture: `try-with-basic.tur` -- `try-with` sugar works for Write/Read effects
- [x] Fixture: `try-with-nested.tur` -- nested `try-with` handlers

### ER6 -- `--dump-effects` flag

- [x] Add `--dump-effects` flag to `CompilerOptions`
- [x] After `effect_check_pass`, print every top-level `defn`'s inferred effect row in format: `fn-name : #{Effect1 Effect2}` (or `#{}` for pure)
- [x] Fixture: `effect-dump.tur` -- `--dump-effects` output matches expected rows

### ER6 -- `--lint-effects` flag

- [ ] Add `--lint-effects` flag; scan for unannotated functions with non-empty inferred rows and emit advisory warnings (similar to `--lint-unsafe`)

### ER6 -- Static one-shot enforcement for `resume`

- [x] Mark `k` in handler clauses as a move-only binding (`TY_CONT` with `CK_MOVE`)
- [x] Wire `EX_RESUME` and `EX_DISCONTINUE` into the borrow checker's move-consumption tracking
- [x] Fixture (negative): `errors/effect-double-resume-static.tur` -- double use of `k` caught at compile time, not runtime

### ER6 -- `cont?` predicate

- [x] Implement `(cont? x)` as `EX_CONT_PRED`; lower to a null-check on the continuation pointer
- [x] Fixture: `effect-cont-pred.tur` -- `cont?` returns correct values (extends existing fixture)

### ER6 -- Priority stdlib effect row annotations

- [x] Annotate all `defn`s in `stdlib/effects.tur` with effect rows
- [x] Annotate all `defn`s in `stdlib/vec.tur` with effect rows
- [x] Annotate all `defn`s in `stdlib/log.tur` with effect rows
- [x] Annotate all `defn`s in `stdlib/async.tur` with effect rows
- [x] Annotate all `defn`s in `stdlib/thread.tur` with effect rows
- [x] Fixture: `stdlib-effects-annotated.tur` -- stdlib `Write`/`Read`/`Fail`/`Log`/`Abort` functions all have row annotations; callers propagate correctly

### ER6 -- IDE / tooling

- [ ] `--check` mode reports effect-row errors without emitting code
- [ ] Language server hover: show inferred effect row for a `defn`
- [ ] Inline hint: display `#{...}` annotation suggestion for unannotated effectful functions (under `--strict-effects`)

---

## ET0 -- Effect Row Syntax in All Type Positions

> No prerequisites beyond ER0--ER6. Can proceed in parallel with HRT development.

### ET0-A -- `(fn [...] #{...} :T)` in type positions

- [x] Update `src/reader.c`: extend `fn` in type context to accept optional `#{...}` between arg list and return type: `(fn [arg-types] #{effect-row} ret-type)`
- [x] Update `elab_type_expr` in `src/elab.c` to parse `#{...}` from `fn` type AST node and store in resulting `TY_FN`'s `effect_row` field
- [x] Confirm `type_equal` and `type_is_subtype` work for the annotated function type case

### ET0-B -- Effect row in type aliases

- [x] `(deftype PureFunc [a b] (fn [a] #{} b))` -- verify empty row propagates when alias is expanded in `elab_type_expr`
- [x] `(deftype IofuncOf [a b] (fn [a] #{Io} b))` -- expanding alias must produce `TY_FN` with `effect_row = #{Io}`

### ET0-C -- Effect row in struct fields

- [x] Struct field typed `(fn [] #{Write} :nil)` stores the effect row in the field's `Type`
- [x] When the field is accessed and called, the caller's inferred row picks up `#{Write}`
- [x] Verify codegen ignores effect rows (erased before `effect_lower.c` emits C)

### ET0-D -- Fixtures

- [x] Fixture: `effect-fn-type-annot.tur` -- `(fn [int] #{Write} int)` in a type annotation accepted; calling it propagates `#{Write}` to the caller
- [x] Fixture: `effect-type-alias.tur` -- type aliases with explicit effect rows expand correctly
- [x] Fixture: `effect-struct-field-row.tur` -- struct with an effectful function field; calling the field propagates the declared row
- [x] Fixture (negative): `errors/effect-fn-type-mismatch.tur` -- assigning a `#{Write}` function to a `#{}` field or variable emits `TUR-E0009`

---

## ET1 -- Effect Row Type Checking

> Requires ET0. Can proceed in parallel with HRT development.

### ET1-A -- Subsumption at let-bindings

- [x] When `(let [f expr] ...)` binds `f` to a function with a declared type annotation, check `inferred_row(expr) ⊆ declared_row(annotation)`
- [x] Emit `TUR-E0009` on failure with binding name in error message

### ET1-B -- Subsumption at if/match branches

- [x] Inferred row of an `if` expression is the union of both branches' rows
- [x] Same rule for `match` -- union of all arm rows
- [x] Verify `collect_effects_in_expr` correctly unions across `EX_IF` / `EX_MATCH`; fix if not

### ET1-C -- Row reduction at `handle` sites

- [x] When elaborating `(handle body clause ...)`:
  - [x] Compute the inferred row of `body`
  - [x] Subtract each handled effect from that row (`effect_row_subtract` -- add to `src/effect.h`)
  - [x] The residual row becomes the inferred row of the entire `handle` expression
- [x] Add `effect_row_subtract` to `src/effect.h`
- [x] Emit `TUR-W0033` ("handler clause for `Foo` is unreachable") when body does not actually perform a handled effect

### ET1-D -- Row reduction at `try-with` sites

- [x] Confirm that `try-with` (ER6) desugars to `handle` before ET1 reduction; verify row reduction applies automatically

### ET1-E -- Do-sequence row accumulation

- [x] In `(do expr1 expr2 ...)`, inferred row is the union of all sub-expression rows
- [x] Verify `collect_effects_in_expr` already unions across `EX_DO` sequences; add fixture if not confirmed

### ET1-F -- Fixtures

- [x] Fixture: `effect-let-subsumption.tur` -- `let`-binding with explicit effect annotation is enforced
- [x] Fixture: `effect-if-union.tur` -- `if` with mixed-effect branches reports union row
- [x] Fixture: `effect-handle-reduce.tur` -- `handle` correctly removes handled effects from enclosing row
- [x] Fixture: `effect-do-union.tur` -- `do` sequences accumulate all sub-expression rows
- [x] Fixture (negative): `errors/effect-handle-unreachable.tur` -- `TUR-W0033` for unreachable handler clause
- [x] Fixture (negative): `errors/effect-let-row-mismatch.tur` -- `TUR-E0009` for over-constrained `let` binding

---

## GATE -- Effect Row Occurs Check (prerequisite for ET2)

> Must be completed and passing before ET2 work begins.

- [x] Add `effect_row_occurs(EffectRowVar*, EffectRow*)` to `src/effect.h`
- [x] Call `effect_row_occurs` inside `effect_row_unify` before committing any substitution
- [x] Emit `TUR-E0254` ("infinite effect row") on occurs-check failure
- [x] Fixture (negative): `errors/effect-row-occurs.tur` -- occurs-check failure produces `TUR-E0254`

---

## ET2 -- Effect Polymorphism (`forall [e]`)

> **Requires:** HRT Phase HRT1 (Rank-2 types) + GATE above.

### ET2-A -- `forall [e]` in type expressions

- [x] `src/reader.c`: parse `(forall [e1 e2 ...] type-expr)` as `EX_FORALL_ROW` (or extend HRT `TY_FORALL` with `TK_EFFECT_ROW` kind marker)
- [x] `elab_type_expr`: elaborate `(forall [e] ...)` into `TY_POLY_ROW` (or extend `TY_FORALL` to carry row-kinded binders)
- [x] Extend the kind system to track `Row` kind (distinct from `*`) for row-kinded binders

### ET2-B -- `EffectRowVar` with universal scope

- [x] Add `ERK_POLY_VAR` to `src/effect.h` (or reuse `ERK_VAR` with a universally-quantified flag) to distinguish per-call-site row vars (ER2) from universally quantified ones (ET2)
- [x] During `forall [e]`-type instantiation, generate fresh `ERK_VAR` variables (same as HRT type-variable instantiation)
- [x] Instantiation and generalisation follow standard Hindley-Milner scheme applied to row kinds

### ET2-C -- Row-variable unification during instantiation

- [x] When `(forall [e] ...)` type is applied, instantiate `e` with a fresh row variable and pass to `effect_row_unify`
- [x] After unification, apply substitution to both function body and call site's inferred row
- [x] Row variables remaining free after function returns are generalised (become universally quantified in inferred type)

### ET2-D -- Effect polymorphism in typeclasses

- [x] `defclass` method signatures may include `forall [e]` row quantifiers (e.g. `Traversable.traverse`)
- [x] `definstance` bodies checked against the instantiated row for the particular instance's concrete effects (builds on ER3)

### ET2-E -- Inference without explicit `forall`

- [x] When a `defn` has a free row variable `#{e}` in its inferred row and `e` is not bound at the call site, generalise `e` to a universally quantified row variable automatically (Hindley-Milner style)
- [x] Under `--strict-effects` (implied by `-Xeffect-types`), emit `TUR-W0034` ("row variable `e` was generalised; consider adding explicit `forall [e]` annotation")

### ET2-F -- Fixtures

- [x] Fixture: `effect-poly-map.tur` -- effect-polymorphic `map` accepted; caller's inferred row matches mapped function's row
- [x] Fixture: `effect-poly-bracket.tur` -- `with-resource` pattern with `forall [e]` callback; row propagates through
- [x] Fixture: `effect-poly-typeclass.tur` -- `Traversable` instance with polymorphic `traverse`; row propagates to caller
- [x] Fixture: `effect-poly-infer.tur` -- `forall [e]` inferred without explicit annotation
- [x] Fixture (negative): `errors/effect-poly-escape.tur` -- row variable escaping its scope is rejected

---

## ET3 -- Handler Typing

> Requires ET2.

### ET3-A -- `TY_HANDLER` type kind

- [x] Add `TY_HANDLER` to `src/types.h` with fields: `effect_name`, `value_type`, `cont_arg_type`, `result_type`
- [x] Add `type_make_handler(...)` constructor in `src/types.c`
- [x] Codegen: emit `TY_HANDLER` as a C struct (or function pointer bundle)

### ET3-B -- Handler clauses as first-class values (typed handler structs)

- [x] A handler is a regular Turmeric struct with a `TY_HANDLER`-typed field holding a function pointer matching the handler signature
- [x] Handlers can be named, bound, and passed as struct values; composition is manual (call the struct's field)
- [x] Reuse existing struct and function pointer machinery

### ET3-C -- Handler typing in `handle` expressions

- [x] When elaborating `(handle body clause ...)`:
  - [x] Elaborate each clause to get its `TY_HANDLER` type
  - [x] Check that handled effects in clauses cover some subset of the body's inferred row
  - [x] Residual row (body row minus handled effects) becomes the inferred row of the `handle` expression
  - [x] `result_type` of each handler clause must match the declared return type of the `handle` expression

### ET3-D -- Handler subtyping

- [x] Extend `type_is_subtype` for `TY_HANDLER`: value type is contravariant, result type is covariant

### ET3-E -- Handler composition

- [x] Implement `(compose-handlers h1 h2)` in `stdlib/effects.tur` (or as a macro) using the `TY_HANDLER` type
- [x] Type-check that effect sets of `h1` and `h2` are disjoint; emit `TUR-E0251` on overlap

### ET3-F -- Fixtures

- [x] Fixture: `effect-handler-type.tur` -- `(handler Write string unit)` type is well-formed; value assigned to a variable
- [x] Fixture: `effect-handler-compose.tur` -- two handlers for different effects composed; caller's row is reduced by the union
- [x] Fixture: `effect-handler-subtype.tur` -- handler subtyping accepted for widened result type
- [x] Fixture (negative): `errors/effect-handler-overlap.tur` -- `TUR-E0251` for composed handlers with overlapping effects
- [x] Fixture (negative): `errors/effect-handler-result-mismatch.tur` -- handler clause return type does not match `handle` expression type

---

## ET4 -- Stdlib, Effect Hierarchy, Tooling, and `-Xeffect-types`

> Requires ET0--ET3.

### ET4 -- Effect hierarchy in stdlib

- [x] Define effect hierarchy using `defeffect` with `^extends` annotation (syntax TBD in implementation)
- [x] Define the standard lattice in stdlib: `Total < Pure < IO (Read, Write, Network, Filesystem)`, `Async (Spawn, Await)`, `Fail (Panic, Throw)`, `Log (Debug, Info, Warn, Error)`, `Unsafe`
- [x] Update `effect_row_is_subset` to respect the hierarchy (performing `Write` satisfies a context requiring `IO`)
- [ ] Document hierarchy in `docs/guides/effects-system-guide.md` and `docs/guides/custom-effects-tutorial.md`
- [x] Fixture: `effect-hierarchy.tur` -- a function performing `Write` satisfies a context requiring `#{IO}` via the hierarchy

### ET4 -- Full stdlib annotation

- [x] Annotate all public `defn`s in `stdlib/list.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/vec.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/map.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/hamt.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/result.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/option.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/free.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/fix.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/async.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/thread.tur` with effect rows
- [x] Annotate all public `defn`s in `stdlib/stm.tur` with effect rows
- [x] Enforce `#{}` on all pure stdlib functions; enforce `#{Write}` / `#{Read}` / `#{Io}` on I/O functions
- [x] Fixture: `effect-stdlib-pure.tur` -- verifies `list/map`, `option/map`, etc. are inferred as `#{}`
- [x] Fixture: `effect-stdlib-io.tur` -- verifies `println`, `file/read`, etc. carry explicit rows

### ET4 -- New error codes (`TUR_E0250`--`TUR_E0299`)

- [x] `TUR-E0250` -- `forall [e]` row variable escapes its quantifier scope
- [x] `TUR-E0251` -- composed handlers have overlapping effect sets (also needed in ET3)
- [x] `TUR-E0252` -- handler clause result type mismatch
- [x] `TUR-E0253` -- effect not in scope at perform site (private or undeclared)
- [x] `TUR-E0254` -- `forall [e]` instantiation produces infinite row (occurs check -- also needed in GATE)
- [ ] Add `tur explain TUR-E025x` entries for all codes
- [ ] Fixture: `effect-error-codes.tur` -- golden-output tests for each new error code

### ET4 -- `-Xeffect-types` feature flag

- [x] Add `-Xeffect-types` compiler flag in `src/main.c` / `src/compiler_options.h`
- [x] When `-Xeffect-types` is active: ET0--ET4 checks enforced; `--strict-effects` implied; `forall [e]` syntax accepted; `TY_HANDLER` available
- [x] When `-Xeffect-types` is absent: system falls back to ER0--ER6 behaviour
- [x] Fixture: `effect-flag-off.tur` -- without `-Xeffect-types`, `forall [e]` syntax is rejected gracefully

### ET4 -- Tooling updates

- [x] Update `--dump-effects` output to show quantified rows: `run-twice : forall [e]. (fn [(fn [] #{e} int)] #{e} int)`
- [ ] Language server hover: show full effect-polymorphic type including quantified rows (if LSP is available)
- [ ] `--check` mode includes ET0--ET4 errors when `-Xeffect-types` is set

---

## LC0--LC3 -- Linear Continuations

> **Requires:** Linear Types (LT0--LT4) stable + Effect Types (ET0--ET4) complete.
> **Target:** v5 or later.

### LC0 -- `cont_kind` field and annotation parsing

- [x] Add `cont_kind : CopyKind` field to `HandlerType` (`TY_HANDLER`) in `src/types.h`; default `CK_UNIQUE`
- [x] Parse `^linear k` annotation in `src/reader.c`; set `CK_LINEAR` on the `k` binding
- [x] Parse `^unsafe-multishot k` annotation in `src/reader.c`; set `CK_COPY` on the `k` binding (no aliasing checks)
- [x] Emit `TUR_W03xx` at every `^unsafe-multishot` use site ("unsafe-multishot continuation -- ownership not tracked")

### LC1 -- Thread `cont_kind` into the elaborator

- [x] In the elaborator's symbol table, set the `CopyKind` of `k` from `cont_kind` when processing a handler clause
- [x] `CK_UNIQUE`: dropping `k` without resuming is permitted; using `k` twice is `TUR_E0201`
- [x] `CK_LINEAR`: dropping `k` without resuming is `TUR_E0100`; using `k` twice is `TUR_E0101`

### LC2 -- Usage tracking and warnings

- [x] Reuse `UsageState` and `AliasState` from substructural framework (ST0--ST1) to track `k` consumption
- [x] Emit `TUR_W03xx` at every `^unsafe-multishot` use site (if not already done in LC0)
- [x] Fixture: `effect-cont-unique.tur` -- single resume under `CK_UNIQUE` accepted
- [x] Fixture (negative): `errors/effect-cont-double-resume.tur` -- `TUR_E0201` for two resumes under `CK_UNIQUE`
- [x] Fixture: `effect-cont-linear.tur` -- exactly-once `^linear k` accepted
- [x] Fixture (negative): `errors/effect-cont-linear-drop.tur` -- `TUR_E0100` for dropping `^linear k`
- [x] Fixture: `effect-cont-abort.tur` -- dropping `k` (aborting) under `CK_UNIQUE` is accepted

### LC3 -- Stdlib migration to `^unsafe-multishot`

- [ ] Annotate backtracking handlers in `stdlib/logic.tur` with `^unsafe-multishot`
- [ ] Annotate any multi-shot async combinators in `stdlib/async.tur` with `^unsafe-multishot`
- [ ] Annotate any other multi-shot stdlib handlers with `^unsafe-multishot`
- [ ] Add `tur explain` entries for `TUR_E0100`, `TUR_E0101`, `TUR_E0201` continuation errors

---

## MS0--MS4 -- Multi-Shot Continuations

> **Requires:** LC0--LC3 stable + `^unsafe-multishot` escape hatch in use in stdlib.
> **Target:** v5 or later.

### MS0 -- Runtime snapshot infrastructure

- [ ] Implement `tur_continuation_snapshot(TurClosure* k)` in `src/runtime/effects.c` -- deep-copies a continuation closure for multi-shot resume
- [ ] Ensure all `CK_COPY` closure capture types support bitwise copy
- [ ] Handle `rc<T>` captures: increment refcount in snapshot
- [ ] Fixture: `multishot-snapshot.tur` -- two independent resumes produce independent results

### MS1 -- `CK_MULTISHOT` kind and `^multishot` annotation

- [ ] Add `CK_MULTISHOT` to `CopyKind` enum in `src/types.h`
- [ ] Parse `^multishot k` in `src/reader.c`; set `CK_MULTISHOT` on the `k` binding in the symbol table
- [ ] `resume` on `CK_MULTISHOT` binding: emit `tur_continuation_snapshot` call in codegen; do not mark `k` consumed
- [ ] `UsageState` for `CK_MULTISHOT`: `USED_MANY` is not an error

### MS2 -- Closure capture analysis

- [ ] At a `^multishot k` handler clause, walk the handler body and collect all free variable captures
- [ ] For each capture, check its `CopyKind`: `CK_COPY` and `CK_MULTISHOT` are allowed; `CK_UNIQUE` or `CK_LINEAR` emit `TUR_E0500`
- [ ] Emit `TUR_E0501` if `^multishot` annotation is used outside a handler continuation binding
- [ ] Emit `TUR_E0502` if `resume k` where `k` is `CK_MULTISHOT` appears inside an `atomic` expression
- [ ] Fixture: `multishot-copy-capture.tur` -- `CK_COPY` capture in `^multishot` handler accepted
- [ ] Fixture (negative): `errors/multishot-unique-capture.tur` -- `CK_UNIQUE` capture rejected with `TUR_E0500`
- [ ] Fixture (negative): `errors/multishot-linear-capture.tur` -- `CK_LINEAR` capture rejected with `TUR_E0500`

### MS3 -- Stdlib migration from `^unsafe-multishot` to `^multishot`

- [ ] Replace `^unsafe-multishot` with `^multishot` in `stdlib/logic.tur`; confirm capture analysis passes
- [ ] Replace `^unsafe-multishot` with `^multishot` in `stdlib/async.tur`; confirm capture analysis passes
- [ ] Replace `^unsafe-multishot` at all other stdlib sites; confirm capture analysis passes

### MS4 -- `^unsafe-multishot` deprecation and removal

- [ ] Emit `TUR_W0400` ("use `^multishot` instead of `^unsafe-multishot`") on any remaining `^unsafe-multishot` annotation
- [ ] Remove `^unsafe-multishot` parsing in the following major version
- [ ] Add `tur explain TUR_E0500`, `TUR_E0501`, `TUR_E0502`, `TUR_W0400` entries

---

## Dependency Summary

```
ER1--ER4 remaining tasks
  |
  ER6 (try-with, --dump-effects, --lint-effects, static one-shot, cont?, stdlib annot.)
    |
    ET0 (effect rows in all type positions)  ←── can run parallel to HRT
      |
      ET1 (expression-level row checking; handle reduction)  ←── can run parallel to HRT
        |
        GATE: effect_row_occurs check (TUR-E0254 fixture must pass)
          |
          ET2 (forall [e] polymorphism)  ←── REQUIRES HRT Phase HRT1
            |
            ET3 (handler typing; TY_HANDLER)
              |
              ET4 (stdlib, hierarchy, tooling, -Xeffect-types)
                |
                LC0--LC3 (linear continuations)  ←── REQUIRES LT0--LT4 stable
                  |
                  MS0--MS4 (multi-shot continuations)
```
