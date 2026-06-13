---
title: Spices Type-Features Uplift Plan
category: Planning
description: A staged pass over the existing spices (everything outside `ecs/` and `ecs-raylib/`) to apply the typeclass / row / substructural / opaque-newtype / sized-type / HKT machinery that the ECS work proved out.
---

# Spices Type-Features Uplift -- Plan

## Goal

The ECS spice (E1, shipped 2026-06-11) was a forcing function: it landed
variadic HKT rows, row-polymorphic `defn`/`fn`, phantom row arguments
through codegen, coherent typeclass dispatch, and the substructural
`^&`/`^&in`/`^&out`/`^borrow` capabilities all the way through
elaboration. Those features now sit underused in the rest of the spice
tree, which was written against the older surface and still leans on
`:int` handles, ad-hoc dispatch, and stringly-typed schemas.

This plan is a **staged uplift** of the remaining spices to use those
features where they buy real safety -- not a stylistic rewrite. Each
phase ships independently; nothing here is a hard prerequisite for
anything else.

## Non-goals

- No refinement types -- not shipping; do not gate work on them.
- No API renames-for-the-sake-of. Touching a public function means a
  measurable safety or ergonomics win.
- No fixture mass-regen as a side effect of a "style" pass. If a phase
  forces a regen, it lands in the same PR per the CLAUDE.md rule.
- `ecs/` and `ecs-raylib/` are out of scope -- they're already on the
  new surface.

## Rubric: when does a feature pay rent?

Before recommending a change, the opportunity has to clear a bar.
"Could be a typeclass" is not enough; "currently has N parallel
functions that branch on a runtime tag and we'd collapse them" is.

| Feature | "Pays rent" criterion |
|---|---|
| `defopaque` newtype | Two `:int` handles of different kinds are passed through the same call sites today and a mix-up would compile but break at runtime. |
| Substructural `^&`/linear | A handle has a `close`/`free`/`destroy` peer and use-after-close is currently a runtime-only error. |
| Typeclass + coherent dispatch | There are ≥3 parallel `foo_a / foo_b / foo_c` functions whose only difference is a payload type, and callers branch on which to use. |
| Row-polymorphic / phantom row | A schema or field set is currently threaded as a string-keyed map or runtime tag, and the call site already knows the shape statically. |
| Sized types | A dimension is asserted at runtime today and the caller knows it at compile time (matrix multiply, vec3 cross, fixed-frame audio buffer). |
| HKT `Fix` / `Free` | A recursive AST is hand-rolled with N accessor functions per node kind and at least one pass over it is open-coded. |
| Variadic `& xs : T` | A builder API today takes a `cons`-list of `:int` plus a runtime tag-check, where the elements are actually homogeneous. |

If a candidate fails the rubric, leave it.

## Prerequisites -- main-repo work that should land first

Before kicking off the phases below, a short list of main-repo changes
would dramatically reduce churn during the uplift. They are ordered by
"how many phases does this unblock"; none is a hard blocker -- each phase
can ship without them, just with more rough edges.

### P0 -- Typed-field row literals (`#row{k : T ...}`) -- DONE 2026-06-12

**Was blocking:** U3 entirely, U2 (json instance derivation), a chunk of
U5 (json decoder shape checks).

`#row{...}` now accepts two slot shapes, gated behind `-Xdata-literals`:
- Bare-positional (existing): `#row{Pos Vel}` -- ECS component rows.
- Typed-field (new): `#row{id : int  name : cstr}` -- a parallel
  `field_names[]` array on `TY_TYPEROW` makes two rows with identical
  element types but different names distinct phantom indices.

Decisions taken:
- **Key syntax:** bare ident with the existing `name : type` structural
  colon, matching turmeric's `defn`/`defstruct` field-annotation style.
- **Mixing rejected:** a single literal must be entirely bare *or*
  entirely typed (TUR-E0290). Avoids ambiguous-shape rows.
- **Duplicate field names rejected** at elab (TUR-E0291).
- **`(k in r)` term-level predicate deferred.** U3's per-column
  accessors are hand-typed (`col-id : Frame r -> Col Int32`); generic
  `column-of` helpers stay out of scope until a concrete need shows up.
  ECS-style elaboration-time row-membership remains the only check.

Fixtures landed:
- `tests/fixtures/typed-field-row-accept/` -- typed-field row used as a
  phantom on a row-kinded defstruct, builds + runs.
- `tests/fixtures/errors/typed-field-row-duplicate-name/` -- TUR-E0291.
- `tests/fixtures/errors/typed-field-row-mixed/` -- TUR-E0290.
- `tests/fixtures/errors/typed-field-row-name-mismatch/` -- two rows
  with same element types but different field names refuse to unify
  (TUR-E0001), showing the printer renders both forms distinctly.

Code touchpoints: `src/compiler/types.h` (added `field_names`),
`src/compiler/types.c` (`type_typerow_named`, equality, perm-eq, both
printers), `src/compiler/elab_types.c` (slot detection +
TUR-E0290/E0291 diagnostics). Reader unchanged -- `name : T` parses to
F_SYM + F_TYPE_ANN already.

### P1 -- `defsystem :writes` enforcement (Path A: per-component `WriteCap`)

**Blocks:** nothing in U1-U6 directly, but it's the missing piece of the
ECS plan
([docs/reported/ecs-defsystem-write-caps-not-enforced.md](../reported/ecs-defsystem-write-caps-not-enforced.md))
and it exercises the same substructural machinery U1 leans on for
`Stmt`/`TlsConn`/`Texture`. Shipping it first stress-tests `^&`/linear
across more code paths than ECS alone, and the bugs it surfaces are
cheaper to fix before five spices depend on the same paths.

### P2a -- `derive-json` macro (narrow, ships first)

**Blocks:** U2 json work in its planned form. **Hard prereq for U2.**

Before generalizing to a `derive` framework (P2b), ship the concrete
`derive-json` macro for json's `Encode`/`Decode` classes specifically.
Rationale: a typeclass-based serde surface where every consumer
hand-writes `(definstance Encode MyStruct ...)` is *worse* than the
status quo `to-json-mystruct` per-type function -- you've added
dispatch overhead without removing the boilerplate. The derive macro
is what makes the typeclass collapse pay off.

Doing the narrow version first also de-risks the general `derive`
design. Json forces every interesting case onto the table:

- `defstruct` (product) -- field name -> json key, field type -> recursive encode.
- `defdata` (sum-of-products) -- needs a discriminator key convention
  (`":tag"` field, externally-tagged vs internally-tagged), and the
  constructor list must be available at macro-expansion time. `str->sym`
  (shipped) + the constructor introspection already in `elab_macros.c`
  is sufficient.
- `defopaque` -- by default, refuse: an opaque type's wire representation
  is *not* its carrier. Require the user to write a hand instance, or
  to opt in with `(derive-json MyOpaque :as :carrier)` for the explicit
  "yes, encode as the underlying int/string" case.
- Generic types (`Vec`, `Option`, `Result`, `Map`) -- ship hand-written
  instances in the json spice itself; `derive-json` does not try to
  derive over type constructors in v1.
- Recursive types (`Fix F`) -- defers to the underlying functor's
  instance; lands with U5's `Fix`-based json AST work.

Surface:

```turmeric
(defstruct User [id : Int32  name : Str  email : Str])
(derive-json User)
;; emits both (definstance Encode User ...) and (definstance Decode User ...)

;; opt out of one direction:
(derive-json User :only [:encode])

;; sum types with externally-tagged discriminator (default):
(defdata Event
  (Click  [x : Int32  y : Int32])
  (Scroll [dy : Int32]))
(derive-json Event)
;; encodes as {"Click": {"x": ..., "y": ...}}
```

Open questions to decide *during* P2a (not before):
- Field-name policy: identity, camelCase, snake_case? Default identity,
  override with `:rename-fields :camel`.
- Missing-field policy on decode: error, or use `defstruct` default
  values when present? Default error; opt in with `:optional`.
- Whether to honor a `#[json :skip]` per-field annotation in v1.

These are surface decisions; the elaboration machinery is the same
either way.

Deliverables for P2a:
1. The `derive-json` macro in the json spice (not stdlib -- it depends
   on json's classes).
2. A fixture corpus covering all four cases above (product, sum, opaque
   opt-in, generic via hand-written instance).
3. A "what would generalize" memo at the end -- the bits of P2a that
   should become reusable infrastructure feed directly into P2b.

### P2b -- General `derive` framework

**Blocks:** U2 scaling beyond json (`Eq`, `Show`, future spices).

After P2a ships and its rough edges show, generalize: a `derive` macro
keyed by class name, where each class declares a `derive-template` hook
(an inline-quoted body keyed by the shape of the type). The elaborator
still owns instance resolution -- coherent dispatch stays coherent.

```turmeric
(derive Encode User)
(derive Decode User)
(derive Eq Event)
(derive Show Event)
```

`derive-json` from P2a becomes a thin shim:
`(derive-json T) == (do (derive Encode T) (derive Decode T))`.

The hard part is keeping each class's template ergonomic to *write*.
P2a's experience with json determines whether the template hook is an
inline-quoted body, a callback into the macro, or a per-class
`(definstance Derivable ...)` blob. Don't pick until P2a ships.

### P3 -- Sized-types index made load-bearing (SZ6) -- DONE 2026-06-10

**Was blocking:** U4's hard guarantees.

SZ6 (indexed constructors + erasure), SZ7 (static size-eq/le rejection),
SZ8 (constructor-chain inference + length-polymorphic helpers), and
cross-parameter size-variable unification have all landed (resolved
report: [docs/archive/history/sized-types-phantom-index.md](../archive/history/sized-types-phantom-index.md)).
U4 is now shippable with real elaborator-enforced dimension errors, not
just stable signatures. Remaining gap is the projection-side recovery
tracked in [docs/reported/sz8-projection-size-recovery-gap.md](../reported/sz8-projection-size-recovery-gap.md);
that's a follow-up, not a U4 blocker.

### P4 -- `match-fix` sugar for `Fix`-encoded ASTs

**Blocks:** U5 ergonomics.

`stdlib/fix.tur` exposes `roll`/`unroll`/`cata`/`ana`. A `cata` is
ergonomic when the algebra is one closure; it is *not* ergonomic when
the algebra has to dispatch on the underlying functor's constructors
(every json node kind, every glsl statement form). Right now the
algebra body looks like:

```turmeric
(fn [layer]
  (match (unroll-functor layer)
    (JNull)      ...
    (JBool b)    ...
    (JNum n)     ...
    ...))
```

The `unroll-functor` step is boilerplate the macro layer should hide.
Proposal: `(match-fix x (JNull) ... (JBool b) ...)` that desugars to
the `cata` + match above, with the inner-recursive type erased to
`Fix F` automatically.

This is *just* sugar -- U5 ships without it, with a verbose `cata`. But
"verbose" here means 20-30 lines per traversal across five spices, so
the cost-benefit is good.

### P5 -- Negative-fixture support in `tests/run.sh`

**Blocks:** U1, U3, U4 validation deliverables.

Each of U1/U3/U4 names a "negative fixture" as its deliverable -- code
that *should* fail to compile with a specific error code. The existing
fixture harness checks `expected.txt`/runtime output; it doesn't have a
first-class "this must fail at elaboration with TUR-EXXXX" mode.

Today fixtures lean on `requires.*` markers and per-fixture
`expected.stderr` files. Either is workable for negative tests, but
there is no convention. Propose: a `requires.compile-fails` marker (with
optional `expected.error-code`) so the harness routes the fixture
through `tur check` and asserts a non-zero exit + an expected diagnostic
code. Cheap to implement, and makes every "type system catches this"
claim verifiable.

### P6 -- Opaque-handle FFI shim convention

**Blocks:** U1 hygiene (does not block correctness).

Every U1 target will need an `unwrap-Handle`/`from-int` pair to cross
the FFI boundary -- the C side speaks `int64_t`, the Turmeric side
speaks `(defopaque Handle :int)`. Today each spice rolls its own.
Propose: a single convention documented in
[`developing-spices-guide.md`](../guides/developing-spices-guide.md),
plus a stdlib helper macro `(defopaque-handle Name)` that emits the
opaque + the FFI shim pair + (under `-Xsubstructural`) the consume
marker on the closer. Keeps the U1 PRs small and uniform.

### Prerequisite -> phase impact table

| Prereq | Blocks | Phases unblocked |
|---|---|---|
| ~~P0 typed-field rows~~ DONE | -- | U3 unblocked (shipped 2026-06-12) |
| P1 `:writes` enforcement | none directly | Stress-tests substructural for U1 |
| P2a `derive-json` | U2 json (hard prereq) | U2 json collapses to a usable surface |
| P2b general `derive` | U2 beyond json | U2 generalizes to `Eq`/`Show`/future classes |
| ~~P3 SZ6 sized index~~ DONE | -- | U4 errors actually bite (shipped 2026-06-10) |
| P4 `match-fix` sugar | none (sugar only) | U5 readability |
| P5 negative-fixture harness | U1/U3/U4 validation | "negative fixture" deliverables become real |
| P6 FFI shim convention | none (hygiene) | U1 PRs stay small |

### What can ship in parallel with the phases

P1 (`:writes`) and P5 (negative fixtures) can land any time relative
to the phases without forcing a phase rewrite -- they upgrade
guarantees rather than change signatures. (P0 / P3 already landed.)
P2a (`derive-json`), P4 (match-fix), and P6 (FFI shim) all change
*surfaces* and should land before their dependent phase to avoid
double-touching the same spice files. P2b (general `derive`) can land
after U2 ships -- the hand-written `definstance` fallback covers the
gap for the other U2 typeclasses (`Renderer`, `Handler`, `Color`)
which only need a handful of instances each.

## Phasing

### Phase U1 -- Resource-handle safety (opaque + substructural)

**Goal:** make handle confusion and use-after-close into compile-time
errors. Highest safety-per-line ratio of any phase.

Targets (in priority order; each is one small PR per spice):

1. **`opengl`** -- separate `Shader`, `Program`, `Buffer` (`VBO`/`VAO`/`EBO`),
   `Texture`, `Framebuffer` opaques; the bind/use/draw paths take `^&`
   borrows. Today every handle is `int64_t` in
   `spices/opengl/opengl__buffers.h`, `opengl__shaders.h`,
   `opengl__textures.h` -- swapping a shader id for a buffer id is a
   silent miscompile at the call site.
2. **`sqlite`** -- `Db`, `Stmt`, `Row` opaques. `Stmt` is **linear**
   (must be `finalized`); `Row` is `^&` of the underlying step. The
   `spices/sqlite/sqlite__db.h` surface currently accepts any
   `int64_t` for `db_prepare`/`db_query`.
3. **`postgres`** -- `Conn`, `Result`, `Stmt` opaques. Mirrors sqlite.
4. **`tls`** -- `TlsConn` linear with required `shutdown`; `read`/`write`
   take `^&`. Today `spices/tls/tls__conn.h` happily takes a closed
   conn.
5. **`valkey`** -- `Client` opaque; pipelined replies are `^&out` so
   you can't read them twice or drop them on the floor.
6. **`raylib`** + **`sdf-raylib` downstream surface** --
   `Texture`, `Sound`, `Image`, `Model` opaques on the base raylib
   spice (these already have C structs but the Turmeric surface
   stores them as `:int`); `^&` on the draw-call path. The
   `sdf-raylib` spice
   ([archived plan](../archive/history/solid-modeling-sdf-raylib-plan.md),
   Phases 1-5 complete 2026-05-28) shipped its own raylib touch
   points -- `Shader`, `RenderTexture`, GLSL-emitted fragment shaders
   exposed through `raylib/integration.tur` -- and those need the
   same `defopaque` + linear/`^&` treatment in this PR, not deferred.
   File-by-file scope:
   - Base raylib: `Texture`, `Sound`, `Image`, `Model`, `Font`,
     `Camera`, `Color`.
   - sdf-raylib's `raylib/integration.tur` and `glsl/codegen.tur`:
     `Shader` (linear -- `UnloadShader` consumes), `RenderTexture`
     (linear -- `UnloadRenderTexture` consumes), `Mesh`/`Material`
     if they're exposed (currently inline C; check before annotating).
   - sdf-raylib's `ColoredSDF` handle: leave alone -- it's already an
     opaque-flavored AST and not a raylib resource; reshape that under
     U5 (Fix-based AST) instead.

Rule of thumb: if the C side has a `*_close`/`*_unload`/`*_finalize`,
the Turmeric side gets a linear opaque.

Validation: every existing call site of the touched spice must compile
unchanged after a one-line `defopaque` shim (the opaques carry `:int`
so coercions stay free); the win is on *new* call sites and on
intentionally broken ones in a regression fixture.

### Phase U2 -- Typeclass collapse of parallel dispatch

**Goal:** replace N parallel functions branching on a payload type with
one typeclass + N instances, picking up coherent dispatch.

Targets:

1. **`ansi`** -- `ansi__color.c` has parallel `fg4/bg4/fg8/bg8/fg24/bg24`.
   One `Color` typeclass with instances `Color4`, `Color8`, `Color24`
   collapses six functions to two (`fg`, `bg`), and the depth becomes
   a *type-level* property of the color value so a 24-bit color cannot
   be passed where a 4-bit one is expected by a terminal that doesn't
   advertise truecolor.
2. **`plot`** -- `plot__core.c` dispatches on backend (canvas vs. PNG
   vs. notebook-inline). One `Renderer` class with instances per
   backend; the plot-building DSL becomes generic in `Renderer`.
3. **`json`** -- `Encode`/`Decode` typeclasses replace the hand-written
   `to_json_*`/`from_json_*` per-type pairs. The instance for a
   `defstruct` can be derived (manual `definstance` today; a
   `derive-json` macro is a separate ticket).
4. **`http`/`httpd`** -- `Handler` class with one method
   `handle : Req -> Resp`; request/response codecs become instances of
   the json `Encode`/`Decode` classes from (3), so handler signatures
   read like `handler : (Encode Req, Decode Resp) => ...`.

Validation: keep the legacy entry points as thin wrappers that call
into the typeclass dispatch; delete them at the end of the phase once
no in-tree caller remains.

### Phase U3 -- Row-typed schemas

**Goal:** carry "what's in this row/header/column-set" at the type
level using the same `#row{...}` phantom machinery the `Query` value
uses in ECS.

Targets:

1. **`frame`** -- `Frame<#row{x:Int32 y:Float64 ...}>`. `frame__schema.h`
   today resolves columns by string name at runtime; the call site
   almost always knows the schema. `column-of` becomes
   `col : Frame r -> (k in r) -> Col t` with `k in r` proved by row
   membership.
2. **`postgres`/`sqlite`** -- `Result<#row{col1:t1 col2:t2}>` and
   `Stmt<#row{p1:t1 ...} #row{c1:t1 ...}>` (params row, columns row).
   Binding the wrong param type or reading the wrong column type
   becomes a type error. Composes with U1's opaques.
3. **`http`/`httpd`** -- `Request<#row{headers...}>` / `Response<...>`.
   The row is the *required* headers; missing headers are an
   unbound-name error at the call site instead of `None` at runtime.
4. **`json`** -- object shapes as rows: a decoder for
   `#row{name:Str age:Int}` rejects payloads missing those keys at
   the boundary, not three frames later.

Validation: a fixture per target that *fails to compile* with a
known-wrong row -- the row check is the deliverable, not the runtime
behavior.

#### U3 decoder model -- borrow from `stdlib/schema.tur`

The static row in `Result<#row{...}>` describes *what the caller
asked for*; the wire actually delivers tagged bytes. Somewhere the
two have to meet, and that's the decoder. `stdlib/schema.tur` is a
near-fit for the shape U3 wants and is worth copying in spirit rather
than reimplementing from scratch:

- **Primitive vocabulary already settled.** Schema commits to
  `:cstr`/`:int`/`:float`/`:bool`/`:null` as its scalar set
  (`SCHEMA_STR`/`_INT`/`_FLOAT`/`_BOOL`/`_NIL` discriminants in
  `stdlib/schema.tur:36-49`). U3 should pick the same primitives so
  json, postgres, sqlite, and http all share one decode model rather
  than each spice inventing its own scalar enum.
- **Validation-applicative error accumulation.** `SCHEMA_AP`
  (`stdlib/schema.tur:78`) accumulates *all* per-field failures with
  dot-separated paths instead of failing fast. This is exactly the
  behavior a "decode this whole pg row" pass wants: a row with two
  wrong-typed columns should report both, not just the first.
- **Boundary anchored on a tagged representation.** Schema decodes
  off the tagged JSON node (kind = 0..6), because raw Turmeric int64s
  carry no runtime type tag. U3's postgres/sqlite decoders need the
  same anchor -- the wire protocol's OID tags for postgres, the
  column-type byte for sqlite -- so the per-row decoder is a function
  `Row -> Result<#row{...}, Vec<DecodeError>>` shaped the same way
  `schema-decode` is.

What U3 adds on top of `tur/schema`:

- **The static row drives schema construction.** Today `tur/schema`
  schemas are built imperatively with `schema/object-new` +
  `schema/field`. Under U3, the row literal `#row{id:Int32 name:Str}`
  is *both* the static type carried in the `Result` phantom and the
  blueprint for a derived runtime decoder. A `derive-decoder` macro
  (sibling of P2a `derive-json`) walks the row at elaboration time
  and emits the equivalent `schema/object-new` + `schema/field` calls
  -- so the runtime decoder is generated from the static row, and the
  two cannot disagree.
- **One decoder model across U3 backends.** json's `Decode` typeclass
  from P2a, postgres's row decoder, sqlite's row decoder, and http's
  header decoder should all return `Result<T, Vec<DecodeError>>` with
  the same `DecodeError` shape (path + expected-type + got-tag). A
  caller that needs to switch postgres for sqlite shouldn't have to
  rewrite its error-handling layer.

Open mechanical question (decide in U3, not before): does U3 reuse
`stdlib/schema.tur` directly, or factor out a smaller
`stdlib/decode.tur` core (just the validation-applicative + error
type) and have `tur/schema` rebuild on top of it? The first is faster
to ship; the second is cleaner long-term because `tur/schema`'s
runtime-built schemas and U3's row-derived schemas are genuinely
different use cases sharing a kernel.

### Phase U4 -- Sized types where dimensions are known

**Goal:** push fixed dimensions from runtime asserts into the type.

Targets:

1. **`linalg`** -- `Vec n`, `Mat m n`. `vec-dot`, `mat-mul`, `cross`,
   `transpose` get dimension-correct signatures. SZ6 + cross-parameter
   size-variable unification shipped 2026-06-10 (resolved report
   archived at
   [docs/archive/history/sized-types-phantom-index.md](../archive/history/sized-types-phantom-index.md)),
   so the size index is load-bearing today: a call site that mixes
   `(Vec 3)` with `(Vec 4)` rejects at the elaborator, not at runtime.
2. **`rtaudio`/`wav`** -- buffer types parameterized by frame count.
3. **`raylib`** image/texture -- `Image w h`; sampler functions take
   a sized image.
4. **`c-dsl`** -- `C.array(T, n)` carries `n` as a phantom index so
   `C.index` on a literal-`n` array is bounds-typed.

Validation: a negative fixture per target that *fails to compile*
with mismatched dimensions (now that SZ6 + cross-parameter unification
have shipped), plus "old code still compiles" for the positive cases.

### Phase U5 -- HKT recursion for ASTs

**Goal:** the spices that hand-roll a recursive IR (parse tree,
codegen IR, regex tree, template tree, S-expression tree) re-express
the node as `Fix F` so generic fold/map/cata works without per-node
accessor scaffolding.

Targets:

1. **`json`** -- `JsonNode = Fix (JsonF a)` where
   `JsonF a = Null | Bool b | Num n | Str s | Arr (list a) | Obj (map str a)`.
   The current emit path (`spices/json/json__emit.c`) is a manual
   recursion; a `cata` collapses it.
2. **`c-dsl`** -- `Expr = Fix ExprF` and `Stmt = Fix StmtF`. The
   pretty-printer (`c_dsl__pp.c`) and codegen (`c_dsl__codegen.c`)
   both become `cata`s; the typedef machinery
   (`c_dsl__typedef.c`/`.h`) stops re-implementing recursion.
3. **`glsl`** -- same shape as `c-dsl`; share the cata if profitable.
4. **`scscm`** -- `SExpr = Fix SExprF`. The parser
   (`scscm__parser.h`) currently exposes flat accessors per node
   kind; cata replaces the open-coded walks.
5. **`regex`** -- `Re = Fix ReF` with `ReF a = Lit c | Alt a a |
   Concat a a | Star a | ...`. NFA construction is one `cata`.
6. **`template`** -- node IR via `Fix`; the renderer is a `cata`.

Validation: round-trip parse->print on a fixture corpus per spice. No
codegen-fixture regen for the main turmeric repo (these spices ship
their own tests).

### Phase U6 -- Typed variadic builders

**Goal:** the few "builder API" spices that take heterogeneous tag-erased
arg lists today move to typed `& xs : T` with one instance per builder
kind.

Targets:

1. **`valkey`** -- per-command builder takes `& args : ValkeyArg` where
   `ValkeyArg` is an opaque sum so a wrong-kind arg is a type error,
   not a `WRONGTYPE` response.
2. **`c-dsl` function/struct builders** -- `& fields : CField` /
   `& params : CParam`.

This phase is small; if the rubric only justifies one target, ship
that one and close the phase.

## Sequencing and dependencies

```
U1 (handles)        -- independent, ship first
U2 (typeclass)      -- json instances feed http in U3
U3 (rows)           -- depends on U1 for postgres/sqlite opaques
U4 (sized)          -- independent
U5 (HKT ASTs)       -- json U5 should land before json U2 derives
U6 (variadic)       -- last; smallest
```

Each spice gets its own PR within a phase; phases don't need to be
strictly serialized but the within-spice ordering above matters
(don't switch `postgres` to typed rows before opaques, or you'll
churn the row signatures twice).

## Risks

- **Public-API churn in spices that have external users.** Mitigation:
  every phase keeps the old entry point as a one-line shim until the
  next minor release of that spice.
- **`defopaque` over-narrowing breaks legitimate generic helpers.**
  Mitigation: keep an explicit `unwrap`/`from-handle` escape hatch for
  the FFI-glue files only; lint for it elsewhere.
- **Row-polymorphic codegen bugs.** The ECS work surfaced a relay-vs-
  carrier classifier bug (now fixed -- see
  [docs/reported/row-polymorphic-defn-call-from-row-polymorphic-context-missing-codegen.md](../reported/row-polymorphic-defn-call-from-row-polymorphic-context-missing-codegen.md)).
  Expect more such bugs as U3 lands; budget for one or two main-repo
  fixes per row-using spice and **file them under `docs/reported/`**
  per the CLAUDE.md rule the moment they're seen.
## Out-of-scope follow-ups

- A `derive-json` / `derive-encode` macro that auto-emits a
  `definstance Encode <Struct>` from a `defstruct`. Separately tracked.
- `Fix`/`Free` ergonomics: a `match-fix` sugar that pattern-matches
  through the unfold without explicit `unFix`. Separately tracked.

## Validation harness

Per phase:

1. `bash tests/run.sh` in the main repo: zero `FAIL`.
2. Per spice: that spice's own `tests/` directory, all green.
3. A "negative" fixture per phase: code that *should* fail to compile
   with the new types -- this is the deliverable for U1, U3, U4.

## Open questions

- ~~For U3 row types on `postgres`/`sqlite`, do we carry the column
  types as Turmeric primitives (`Int32`/`Float64`/`Str`) or as opaque
  `PgInt4`/`PgFloat8` newtypes?~~ **Resolved: Turmeric primitives.**
  Ergonomics win; opaque per-vendor scalar types would force every
  domain-level helper through a `pg-int->int32` shim and lose the
  cross-backend uniformity that lets the same row schema describe a
  `Result` from postgres or sqlite (or a json object, or an http
  body). The client/server coercion-mismatch case the opaques would
  have caught is better handled at the *decode* boundary by an
  accumulating validator that reports per-column failures with
  field-path context -- which is what `stdlib/schema.tur` already does
  for json bodies. See "U3 decoder model" below.
- ~~The opaque-handle pass in U1 will touch `raylib` -- coordinate
  with the `sdf-raylib` Phase 2 colored-SDF work.~~ **Resolved:**
  sdf-raylib Phases 1-5 are complete (archived plan:
  [docs/archive/history/solid-modeling-sdf-raylib-plan.md](../archive/history/solid-modeling-sdf-raylib-plan.md)),
  so there is no concurrent work to time against. U1 target 6 now
  enumerates the raylib handles sdf-raylib introduced (`Shader`,
  `RenderTexture`) so they get the same `defopaque` + linear
  treatment in the same PR rather than being left as `:int` islands
  in an otherwise-typed surface.
