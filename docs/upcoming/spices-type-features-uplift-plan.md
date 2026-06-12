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

- No refinement types -- not shipping; do not gate work on them. See
  [docs/reported/refinement-types-not-implemented.md](../reported/refinement-types-not-implemented.md).
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

### P0 -- Typed-field row literals (`#row{k:T ...}`)

**Blocks:** U3 entirely, U2 (json instance derivation), a chunk of U5
(json decoder shape checks).

The shipped row literal accepts bare names -- `#row{Pos Vel}` -- which
is exactly what ECS needs because the component *type* is the only key.
U3's targets (postgres results, sqlite rows, http headers, json objects)
need rows of `key:type` pairs:

```turmeric
(Result #row{id:Int32 name:Str created-at:TimeStamp})
(Request #row{Authorization:Str Content-Type:Str})
```

Two open design questions for the spec:
- Is the key a keyword (`#row{:id Int32 ...}`), a bare ident treated as a
  symbol, or a string? ECS uses bare-ident-as-type-name; for U3 the key
  is *not* a type, so the syntax has to disambiguate.
- Row membership predicate -- do we expose `(k in r)` as a constraint at
  the term level (`col : Frame r -> (k in r) -> Col (lookup k r)`), or
  only at elaboration time via the existing row-membership check? The
  ECS surface only uses the latter; U3's `column-of` needs the former
  or it stays stringly-typed at the value level.

If we ship typed-field rows but defer the `(k in r)` predicate, U3 still
works -- callers write `col-id : Frame r -> Col Int32` per known column
name, which is fine for hand-written DAOs but bad for generic helpers.

### P1 -- `defsystem :writes` enforcement (Path A: per-component `WriteCap`)

**Blocks:** nothing in U1-U6 directly, but it's the missing piece of the
ECS plan
([docs/reported/ecs-defsystem-write-caps-not-enforced.md](../reported/ecs-defsystem-write-caps-not-enforced.md))
and it exercises the same substructural machinery U1 leans on for
`Stmt`/`TlsConn`/`Texture`. Shipping it first stress-tests `^&`/linear
across more code paths than ECS alone, and the bugs it surfaces are
cheaper to fix before five spices depend on the same paths.

### P2 -- `derive` for typeclass instances (`derive-Encode`, `derive-Decode`, ...)

**Blocks:** U2 scaling, U5's "json fixture corpus" validation.

Today every typeclass instance is a hand-written `definstance`. For four
or five in-tree types this is fine -- the json spice has maybe a dozen
encodable types in its test corpus. For *external* users of U2, hand-
writing `definstance Encode <MyAppStruct>` for every domain struct is a
non-starter.

Concrete proposal: a `derive` macro that walks a `defstruct`/`defdata`
declaration at elaboration time and emits the matching `definstance` for
a named class (`Encode`, `Decode`, `Eq`, `Show`). The class itself
declares a `derive-template` hook (an inline-quoted body keyed by the
shape of the type) so each class controls its own derivation -- coherent
dispatch stays coherent because the elaborator still owns instance
resolution.

The hard part is `defdata` -- sum-of-products derivation needs the
constructor list at macro-expansion time. `str->sym` (shipped) plus the
existing constructor introspection in `elab_macros.c` covers it; the
remaining work is the surface.

### P3 -- Sized-types index made load-bearing (SZ6)

**Blocks:** U4's hard guarantees (the *signatures* land without it; the
*errors* do not).

Already tracked under `memory: project_sized_types_phase`. Listed here
only so U4 doesn't ship before someone has decided whether to land SZ6
in the same release train.

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
| P0 typed-field rows | U3, parts of U2/U5 | U3 ships in its planned form |
| P1 `:writes` enforcement | none directly | Stress-tests substructural for U1 |
| P2 `derive` macro | U2 scaling | U2 ships as a public-facing surface |
| P3 SZ6 sized index | U4 guarantees | U4 errors actually bite |
| P4 `match-fix` sugar | none (sugar only) | U5 readability |
| P5 negative-fixture harness | U1/U3/U4 validation | "negative fixture" deliverables become real |
| P6 FFI shim convention | none (hygiene) | U1 PRs stay small |

### What can ship in parallel with the phases

P1 (`:writes`), P3 (SZ6), and P5 (negative fixtures) can land any time
relative to the phases without forcing a phase rewrite -- they upgrade
guarantees rather than change signatures. P0 (typed rows), P2 (derive),
P4 (match-fix), and P6 (FFI shim) all change *surfaces* and should
land before their dependent phase to avoid double-touching the same
spice files.

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
6. **`raylib`** -- `Texture`, `Sound`, `Image`, `Model` opaques (these
   already have C structs but the Turmeric surface stores them as
   `:int`); `^&` on the draw-call path.

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

### Phase U4 -- Sized types where dimensions are known

**Goal:** push fixed dimensions from runtime asserts into the type.

Targets:

1. **`linalg`** -- `Vec n`, `Mat m n`. `vec-dot`, `mat-mul`, `cross`,
   `transpose` get dimension-correct signatures. Sized-types index is
   still phantom (see
   [docs/reported/sized-types-phantom-index.md](../reported/sized-types-phantom-index.md)),
   so this is "ready to inherit guarantees when SZ6 lands" -- but the
   *signatures* are stable now and catch the mix-ups they already
   would.
2. **`rtaudio`/`wav`** -- buffer types parameterized by frame count.
3. **`raylib`** image/texture -- `Image w h`; sampler functions take
   a sized image.
4. **`c-dsl`** -- `C.array(T, n)` carries `n` as a phantom index so
   `C.index` on a literal-`n` array is bounds-typed.

Validation: the phantom index has no runtime cost, so the only
required validation is "old code still compiles". The wins come when
SZ6 makes the index load-bearing.

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
- **Sized-types index is still phantom.** U4 ships the *signatures*
  but the dimension errors only become non-bypassable once SZ6 lands
  (see `memory: project_sized_types_phase`).

## Out-of-scope follow-ups

- A `derive-json` / `derive-encode` macro that auto-emits a
  `definstance Encode <Struct>` from a `defstruct`. Separately tracked.
- `Fix`/`Free` ergonomics: a `match-fix` sugar that pattern-matches
  through the unfold without explicit `unFix`. Separately tracked.
- Refinement-typed `/has` constraints across the whole spice tree --
  v2 once refinement types ship.

## Validation harness

Per phase:

1. `bash tests/run.sh` in the main repo: zero `FAIL`.
2. Per spice: that spice's own `tests/` directory, all green.
3. A "negative" fixture per phase: code that *should* fail to compile
   with the new types -- this is the deliverable for U1, U3, U4.

## Open questions

- Do we land U2's json typeclasses before or after a `derive-json`
  macro? Manual `definstance` is fine for the four or five types the
  in-tree spices care about; external users would want the derive.
- For U3 row types on `postgres`/`sqlite`, do we carry the column
  types as Turmeric primitives (`Int32`/`Float64`/`Str`) or as opaque
  `PgInt4`/`PgFloat8` newtypes? The former is more ergonomic; the
  latter catches client/server type-coercion mistakes.
- The opaque-handle pass in U1 will touch `raylib` -- coordinate with
  the `sdf-raylib` Phase 2 colored-SDF work
  (`memory: project_sdf_raylib_phase`) so we don't double-regen the
  fixture set.
